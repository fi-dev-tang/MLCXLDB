#include "backend/leanstore/storage/record-cache/RecordCache.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheSlabAllocator.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheEntry.hpp"

#include <iostream>
#include<algorithm>
#include <vector>
#include <string>
#include <thread>
#include <cassert>
#include <sys/mman.h>
#include <atomic>
#include <random>
#include <set>
#include <mutex>
#include <iomanip>

using namespace leanstore::storage::recordcache;

// ============================================================
// ANSI 颜色输出
// ============================================================
#define COLOR_GREEN  "\033[32m"
#define COLOR_RED    "\033[31m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RESET  "\033[0m"

// ============================================================
// MockBufferManager
// ============================================================
class MockBufferManager {
public:
    void* record_cache_ptr;
    size_t total_cache_size;

    MockBufferManager(size_t size_in_mb) {
        total_cache_size = size_in_mb * 1024 * 1024;
        record_cache_ptr = mmap(nullptr, total_cache_size,
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (record_cache_ptr == MAP_FAILED) {
            throw std::bad_alloc();
        }
    }

    ~MockBufferManager() {
        if (record_cache_ptr != MAP_FAILED) {
            munmap(record_cache_ptr, total_cache_size);
        }
    }

    uintptr_t pool_start() const {
        return reinterpret_cast<uintptr_t>(record_cache_ptr);
    }

    uintptr_t pool_end() const {
        return pool_start() + total_cache_size;
    }

    // 核心验证：指针是否在 mmap 池内
    bool contains(void* ptr) const {
        uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        return p >= pool_start() && p < pool_end();
    }
};

// ============================================================
// 辅助：打印测试分隔线
// ============================================================
static void print_header(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(60, '=') << std::endl;
}

static void print_pass(const std::string& msg) {
    std::cout << COLOR_GREEN << "  [PASS] " << COLOR_RESET << msg << std::endl;
}

static void print_fail(const std::string& msg) {
    std::cout << COLOR_RED << "  [FAIL] " << COLOR_RESET << msg << std::endl;
}

static void print_info(const std::string& msg) {
    std::cout << COLOR_YELLOW << "  [INFO] " << COLOR_RESET << msg << std::endl;
}

// ============================================================
// TEST 1: 指针范围验证
// 最直接的证明：allocate() 返回的指针必须在 mmap 池内
// 如果 SlabAllocator 内部偷用系统 malloc，指针会落在堆上，
// 与 mmap 池地址范围完全不重叠，立刻被发现。
// ============================================================
void test_pointer_in_pool() {
    print_header("TEST 1: Pointer Range Verification");
    print_info("Allocating entries via SlabAllocator and verifying pointers are inside mmap pool");

    MockBufferManager bm(20); // 20 MB
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    std::pmr::polymorphic_allocator<std::byte> alloc(&allocator);

    print_info("mmap pool range: [" +
               std::to_string(bm.pool_start()) + ", " +
               std::to_string(bm.pool_end()) + ")");

    // 测试多种不同大小，覆盖不同 SlabClass
    std::vector<size_t> test_sizes = {
        24,          // MIN_BLOCK_SIZE，SlabClass[0]
        32,          // SlabClass[1]
        64,          // 中间
        256,         // 常见大小
        1024,        // 1 KB
        4096,        // 4 KB
        16 * 1024    // MAX_BLOCK_SIZE，最大 SlabClass
    };

    bool all_passed = true;

    for (size_t size : test_sizes) {
        // 通过 pmr 接口分配，内部调用 do_allocate
        void* ptr = alloc.allocate_bytes(size, 8);

        bool in_pool = bm.contains(ptr);

        std::cout << "  allocate(" << std::setw(6) << size << " B) -> ptr="
                  << ptr
                  << (in_pool ? COLOR_GREEN " [IN POOL ✓]" COLOR_RESET
                               : COLOR_RED   " [NOT IN POOL ✗]" COLOR_RESET)
                  << std::endl;

        if (!in_pool) {
            all_passed = false;
            // 如果不在池内，说明用的是系统 malloc
            print_fail("Pointer is on system heap, NOT from SlabAllocator's mmap pool!");
        }

        alloc.deallocate_bytes(ptr, size, 8);
    }

    assert(all_passed && "One or more allocations returned pointers outside mmap pool!");
    print_pass("All allocated pointers are inside mmap pool → SlabAllocator is working!");
}

// ============================================================
// TEST 2: 内存池耗尽验证
// 这是与系统 malloc 最本质的区别：
// SlabAllocator 的内存是有界的（来自固定大小的 mmap）。
// 当池子用完时，必须抛出 std::bad_alloc。
// 系统 malloc 不会因为 mmap 池满而失败。
// ============================================================
void test_pool_exhaustion() {
    print_header("TEST 2: Pool Exhaustion Verification");
    print_info("Using a small pool (4 MB) and allocating until exhaustion");
    print_info("SlabAllocator must throw std::bad_alloc when pool is full");
    print_info("(System malloc would never fail due to our mmap pool size)");

    // 故意只给 4 MB，每次分配 256 B
    // 4 MB / 2 MB (HUGE_PAGE_SIZE) = 2 个 Slab
    // 每个 Slab 有 2MB / 256B = 8192 个 block
    // 总共最多分配 2 * 8192 = 16384 个 block
    MockBufferManager bm(4);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    std::pmr::polymorphic_allocator<std::byte> alloc(&allocator);

    constexpr size_t ALLOC_SIZE = 256;
    constexpr size_t HUGE_PAGE = 2 * 1024 * 1024;
    // 4MB 池子，256B 块，最多 2 个 Slab，理论上限
    const size_t theoretical_max = (bm.total_cache_size / HUGE_PAGE) * (HUGE_PAGE / ALLOC_SIZE);

    print_info("Pool size: 4 MB, Allocation size: 256 B");
    print_info("Theoretical max allocations: " + std::to_string(theoretical_max));

    std::vector<void*> ptrs;
    ptrs.reserve(theoretical_max + 10);
    bool got_bad_alloc = false;
    size_t actual_count = 0;

    try {
        while (true) {
            void* p = alloc.allocate_bytes(ALLOC_SIZE, 8);

            // 每次都验证指针在池内
            assert(bm.contains(p) && "Got pointer outside mmap pool during exhaustion test!");

            ptrs.push_back(p);
            actual_count++;

            // 防止无限循环（系统 malloc 永远不会耗尽）
            if (actual_count > theoretical_max + 100) {
                print_fail("Allocated " + std::to_string(actual_count) +
                           " times without exhaustion — likely using system malloc!");
                assert(false && "Pool should have been exhausted by now!");
            }
        }
    } catch (const std::bad_alloc&) {
        got_bad_alloc = true;
    }

    assert(got_bad_alloc && "SlabAllocator never threw bad_alloc — pool exhaustion not enforced!");
    assert(actual_count <= theoretical_max &&
           "Allocated more blocks than pool can hold — SlabAllocator boundary broken!");

    print_info("Actually allocated: " + std::to_string(actual_count) +
               " blocks before exhaustion");

    // 验证：释放后可以重新分配（free_list 回收正常）
    for (void* p : ptrs) {
        alloc.deallocate_bytes(p, ALLOC_SIZE, 8);
    }

    // 释放后应该能再次分配
    void* p_after = alloc.allocate_bytes(ALLOC_SIZE, 8);
    assert(bm.contains(p_after) && "After dealloc, realloc returned pointer outside pool!");
    alloc.deallocate_bytes(p_after, ALLOC_SIZE, 8);

    print_pass("Pool exhausted at " + std::to_string(actual_count) +
               " blocks (≤ " + std::to_string(theoretical_max) + ") → SlabAllocator boundary enforced!");
    print_pass("After deallocate, re-allocation succeeded → free_list recycle works!");
}

// ============================================================
// TEST 3: Slab 对齐与步长验证
// SlabAllocator 以 8B 为步长对齐到 SlabClass。
// 验证：同一 SlabClass 内的连续分配指针差值 == block_size。
// 系统 malloc 的指针间距随机，不符合这个规律。
// ============================================================
void test_slab_alignment_and_step() {
    print_header("TEST 3: Slab Alignment and Step Verification");
    print_info("Allocating multiple blocks of same size, checking pointer spacing == block_size");

    MockBufferManager bm(10);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    std::pmr::polymorphic_allocator<std::byte> alloc(&allocator);

    // 测试几个 size class
    // SlabAllocator 会 align_up 到 8B 边界
    // 例如请求 30B -> align_up -> 32B，block_size = 32
    struct TestCase {
        size_t request_size;
        size_t expected_block_size; // align_up to 8B boundary
    };

    std::vector<TestCase> cases = {
        {24,  24},   // 恰好是 MIN_BLOCK_SIZE
        {25,  32},   // align_up: 25 -> 32
        {30,  32},   // align_up: 30 -> 32
        {32,  32},   // 恰好对齐
        {100, 104},  // align_up: 100 -> 104
        {256, 256},  // 恰好对齐
    };

    bool all_passed = true;

    for (auto& tc : cases) {
        // 连续分配 5 个同 size 的 block
        const int N = 5;
        std::vector<void*> ptrs(N);
        for (int i = 0; i < N; i++) {
            ptrs[i] = alloc.allocate_bytes(tc.request_size, 8);
            assert(bm.contains(ptrs[i]));
        }

        // 排序指针（同一个 Slab 内，应该是连续的）
        std::vector<uintptr_t> addrs(N);
        for (int i = 0; i < N; i++) {
            addrs[i] = reinterpret_cast<uintptr_t>(ptrs[i]);
        }
        std::sort(addrs.begin(), addrs.end());

        // 检查相邻指针的差值是否等于 expected_block_size
        bool spacing_correct = true;
        for (int i = 0; i < N - 1; i++) {
            size_t gap = addrs[i + 1] - addrs[i];
            if (gap != tc.expected_block_size) {
                spacing_correct = false;
                std::cout << "  request=" << tc.request_size
                          << " B, expected block_size=" << tc.expected_block_size
                          << " B, but gap=" << gap << " B" << COLOR_RED " ✗" COLOR_RESET << std::endl;
            }
        }

        if (spacing_correct) {
            std::cout << "  request=" << std::setw(4) << tc.request_size
                      << " B → aligned block_size=" << std::setw(4) << tc.expected_block_size
                      << " B, pointer spacing consistent"
                      << COLOR_GREEN " ✓" COLOR_RESET << std::endl;
        } else {
            all_passed = false;
        }

        for (int i = 0; i < N; i++) {
            alloc.deallocate_bytes(ptrs[i], tc.request_size, 8);
        }
    }

    assert(all_passed && "Slab alignment/step verification failed!");
    print_pass("All size classes show correct 8-byte step alignment → SlabAllocator slab layout verified!");
}

// ============================================================
// TEST 4: 多线程并发分配，验证无指针重叠
// 64 个线程同时分配，每个分配的指针必须：
// (1) 在 mmap 池内
// (2) 全局唯一（无两个线程拿到同一块内存）
// 如果 SlabAllocator 线程安全有问题，会出现重复指针。
// ============================================================
void test_concurrent_no_duplicate_pointers() {
    print_header("TEST 4: Concurrent Allocation - No Duplicate Pointers");
    print_info("64 threads allocating simultaneously, all pointers must be unique and in-pool");

    MockBufferManager bm(500);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    std::pmr::polymorphic_allocator<std::byte> alloc(&allocator);

    const int NUM_THREADS = 64;
    const int ALLOCS_PER_THREAD = 1000;
    const size_t ALLOC_SIZE = 256;

    std::mutex global_ptrs_mutex;
    std::set<uintptr_t> global_ptrs_set;
    std::atomic<int> out_of_pool_count{0};
    std::atomic<int> duplicate_count{0};

    // ============================================================
    // 修复核心：用 barrier 把所有线程的分配和释放完全隔离
    // Phase 1: 所有线程只分配，不释放
    // Phase 2: 统一检查唯一性
    // Phase 3: 所有线程统一释放
    // ============================================================

    // 存储所有线程的分配结果
    std::vector<std::vector<void*>> all_thread_ptrs(NUM_THREADS);

    // Phase 1: 并发分配（所有线程只分配，不释放）
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; ++t) {
            threads.emplace_back([&, t]() {
                all_thread_ptrs[t].reserve(ALLOCS_PER_THREAD);
                for (int i = 0; i < ALLOCS_PER_THREAD; ++i) {
                    void* p = alloc.allocate_bytes(ALLOC_SIZE, 8);

                    // 验证指针在池内
                    if (!bm.contains(p)) {
                        out_of_pool_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    all_thread_ptrs[t].push_back(p);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    print_info("Phase 1 complete: all threads finished allocating");

    // Phase 2: 单线程统一检查唯一性（此时没有任何释放发生）
    {
        for (int t = 0; t < NUM_THREADS; ++t) {
            for (void* p : all_thread_ptrs[t]) {
                uintptr_t addr = reinterpret_cast<uintptr_t>(p);
                auto [it, inserted] = global_ptrs_set.insert(addr);
                if (!inserted) {
                    duplicate_count.fetch_add(1, std::memory_order_relaxed);
                    // 打印重复的指针，方便调试
                    std::cout << COLOR_RED
                              << "  Duplicate ptr: 0x" << std::hex << addr << std::dec
                              << COLOR_RESET << std::endl;
                }
            }
        }
    }
    print_info("Phase 2 complete: uniqueness check done");

    print_info("Total allocations:    " + std::to_string(NUM_THREADS * ALLOCS_PER_THREAD));
    print_info("Out-of-pool pointers: " + std::to_string(out_of_pool_count.load()));
    print_info("Duplicate pointers:   " + std::to_string(duplicate_count.load()));

    // Phase 3: 统一释放
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; ++t) {
            threads.emplace_back([&, t]() {
                for (void* p : all_thread_ptrs[t]) {
                    alloc.deallocate_bytes(p, ALLOC_SIZE, 8);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    print_info("Phase 3 complete: all memory deallocated");

    assert(out_of_pool_count.load() == 0 &&
           "Some pointers are outside mmap pool — SlabAllocator not being used!");
    assert(duplicate_count.load() == 0 &&
           "Duplicate pointers detected — SlabAllocator has thread-safety bug!");

    print_pass("All " + std::to_string(NUM_THREADS * ALLOCS_PER_THREAD) +
               " pointers are unique and inside mmap pool → Thread-safe SlabAllocator confirmed!");
}


// ============================================================
// TEST 5: Deallocate 回收验证（free_list 复用）
// 释放后再分配，新指针必须等于之前释放的指针之一。
// 这证明 free_list 链表真正在工作（LIFO 回收）。
// 系统 malloc 也有类似行为，但关键是指针必须在 mmap 池内。
// ============================================================
void test_deallocate_recycle() {
    print_header("TEST 5: Deallocate Recycle (free_list Verification)");
    print_info("Allocate N blocks, deallocate them, reallocate — new ptrs must match old ptrs");

    MockBufferManager bm(10);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    std::pmr::polymorphic_allocator<std::byte> alloc(&allocator);

    const size_t ALLOC_SIZE = 128;
    const int N = 10;

    // Step 1: 分配 N 个 block，记录地址
    std::vector<void*> first_round(N);
    std::set<uintptr_t> first_round_addrs;

    for (int i = 0; i < N; i++) {
        first_round[i] = alloc.allocate_bytes(ALLOC_SIZE, 8);
        assert(bm.contains(first_round[i]));
        first_round_addrs.insert(reinterpret_cast<uintptr_t>(first_round[i]));
    }
    print_info("First round: allocated " + std::to_string(N) + " blocks");

    // Step 2: 全部释放（回到 free_list）
    for (int i = 0; i < N; i++) {
        alloc.deallocate_bytes(first_round[i], ALLOC_SIZE, 8);
    }
    print_info("Deallocated all " + std::to_string(N) + " blocks back to free_list");

    // Step 3: 再次分配 N 个 block
    std::vector<void*> second_round(N);
    std::set<uintptr_t> second_round_addrs;

    for (int i = 0; i < N; i++) {
        second_round[i] = alloc.allocate_bytes(ALLOC_SIZE, 8);
        assert(bm.contains(second_round[i]));
        second_round_addrs.insert(reinterpret_cast<uintptr_t>(second_round[i]));
    }
    print_info("Second round: allocated " + std::to_string(N) + " blocks");

    // Step 4: 验证第二轮地址与第一轮完全相同（free_list LIFO 复用）
    // 注意：顺序可能不同（LIFO），但集合应该完全相同
    bool sets_equal = (first_round_addrs == second_round_addrs);

    std::cout << "  First  round addresses: ";
    for (uintptr_t a : first_round_addrs) {
        std::cout << std::hex << a << " ";
    }
    std::cout << std::dec << std::endl;

    std::cout << "  Second round addresses: ";
    for (uintptr_t a : second_round_addrs) {
        std::cout << std::hex << a << " ";
    }
    std::cout << std::dec << std::endl;

    assert(sets_equal &&
           "Second round pointers differ from first round — free_list recycle not working!");

    for (int i = 0; i < N; i++) {
        alloc.deallocate_bytes(second_round[i], ALLOC_SIZE, 8);
    }

    print_pass("Second round pointers exactly match first round → free_list LIFO recycle confirmed!");
}

// ============================================================
// TEST 6: 超大分配 fallback 验证
// bytes > MAX_BLOCK_SIZE (16 KB) 时，SlabAllocator 应该
// fallback 到 new_delete_resource，指针不在 mmap 池内。
// 这验证了 do_allocate 的 fallback 路径。
// ============================================================
void test_oversized_fallback() {
    print_header("TEST 6: Oversized Allocation Fallback Verification");
    print_info("Allocating > 16 KB should fallback to system new/delete, NOT from mmap pool");

    MockBufferManager bm(10);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    std::pmr::polymorphic_allocator<std::byte> alloc(&allocator);

    // 在 MAX_BLOCK_SIZE 边界两侧各测一个
    struct TestCase {
        size_t size;
        bool expect_in_pool;
        std::string desc;
    };

    std::vector<TestCase> cases = {
        {16 * 1024,      true,  "MAX_BLOCK_SIZE (16 KB) → in pool"},
        {16 * 1024 + 8,  false, "MAX_BLOCK_SIZE + 8 B  → fallback to system heap"},
        {64 * 1024,      false, "64 KB                 → fallback to system heap"},
        {1024 * 1024,    false, "1 MB                  → fallback to system heap"},
    };

    bool all_passed = true;

    for (auto& tc : cases) {
        void* p = alloc.allocate_bytes(tc.size, 8);
        bool in_pool = bm.contains(p);

        bool result_correct = (in_pool == tc.expect_in_pool);
        std::cout << "  allocate(" << std::setw(8) << tc.size << " B): "
                  << tc.desc << " → "
                  << (in_pool ? "IN POOL" : "SYSTEM HEAP")
                  << (result_correct ? COLOR_GREEN " ✓" COLOR_RESET
                                     : COLOR_RED   " ✗" COLOR_RESET)
                  << std::endl;

        if (!result_correct) all_passed = false;

        alloc.deallocate_bytes(p, tc.size, 8);
    }

    assert(all_passed && "Oversized fallback behavior is incorrect!");
    print_pass("Oversized fallback path verified → do_allocate boundary logic correct!");
}

// ============================================================
// TEST 7: RecordCache 集成测试（端到端）
// 把 SlabAllocator 和 RecordCache 联合起来，
// 验证从分配到插入到查询的完整链路，
// 且所有 entry 指针均在 mmap 池内。
// ============================================================
void test_record_cache_integration() {
    print_header("TEST 7: RecordCache Integration (End-to-End)");
    print_info("Allocate entries via SlabAllocator, insert into RecordCache, verify all ptrs in pool");

    MockBufferManager bm(100);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    std::pmr::polymorphic_allocator<std::byte> alloc(&allocator);
    RecordCache cache(allocator, 16);

    const int NUM_ENTRIES = 100;
    std::vector<std::string> keys(NUM_ENTRIES);
    std::vector<RecordCacheEntry*> entries(NUM_ENTRIES);

    // Step 1: 通过 SlabAllocator 分配 entry，插入 cache
    for (int i = 0; i < NUM_ENTRIES; i++) {
        keys[i] = "key_" + std::to_string(i);
        size_t entry_size = sizeof(RecordCacheEntry) + keys[i].size() + 8;

        entries[i] = reinterpret_cast<RecordCacheEntry*>(
            alloc.allocate_bytes(entry_size, 8)
        );

        // 验证分配的指针在 mmap 池内
        assert(bm.contains(entries[i]) &&
               "Entry pointer is not in mmap pool — SlabAllocator not used!");

        entries[i]->key_length   = keys[i].size();
        entries[i]->value_length = 8;
        entries[i]->setType(RecordCacheType::ReadOnlyMode);

        std::span<const uint8_t> key_span{
            reinterpret_cast<const uint8_t*>(keys[i].data()), keys[i].size()
        };
        bool inserted = cache.InsertOrAssignInRecordCache(key_span, entries[i]);
        assert(inserted == true);
    }
    print_info("Inserted " + std::to_string(NUM_ENTRIES) + " entries (all from mmap pool)");

    // Step 2: 查询，验证返回指针与 SlabAllocator 分配的一致
    int found = 0;
    for (int i = 0; i < NUM_ENTRIES; i++) {
        std::span<const uint8_t> key_span{
            reinterpret_cast<const uint8_t*>(keys[i].data()), keys[i].size()
        };
        RecordCacheEntry* res = cache.GetFromRecordCache(key_span);

        assert(res != nullptr);
        assert(res == entries[i] &&
               "Cache returned wrong pointer — not the one we allocated!");
        assert(bm.contains(res) &&
               "Cache returned pointer is outside mmap pool!");
        assert(res->isExpectedType(RecordCacheType::ReadOnlyMode));
        found++;
    }
    print_info("Found " + std::to_string(found) + "/" + std::to_string(NUM_ENTRIES) +
               " entries, all pointers match SlabAllocator allocations");

    // Step 3: 释放
    for (int i = 0; i < NUM_ENTRIES; i++) {
        size_t entry_size = sizeof(RecordCacheEntry) + keys[i].size() + 8;
        alloc.deallocate_bytes(entries[i], entry_size, 8);
    }

    print_pass("End-to-end: SlabAllocator → RecordCache → verified all pointers in mmap pool!");
}

// ============================================================
// main
// ============================================================
int main() {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "   RecordCacheSlabAllocator Verification Test Suite\n";
    std::cout << "   (Proving SlabAllocator is actually being used)\n";
    std::cout << std::string(60, '=') << std::endl;

    try {
        test_pointer_in_pool();           // TEST 1: 指针必须在 mmap 池内
        test_pool_exhaustion();           // TEST 2: 池子用完必须 bad_alloc
        test_slab_alignment_and_step();   // TEST 3: 8B 步长对齐验证
        test_concurrent_no_duplicate_pointers(); // TEST 4: 多线程无重复指针
        test_deallocate_recycle();        // TEST 5: free_list LIFO 回收
        test_oversized_fallback();        // TEST 6: 超大分配 fallback
        test_record_cache_integration();  // TEST 7: 端到端集成

    } catch (const std::exception& e) {
        print_fail(std::string("Exception: ") + e.what());
        return 1;
    }

    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << COLOR_GREEN
              << "   All 7 Tests Passed — SlabAllocator is genuinely working!\n"
              << COLOR_RESET;
    std::cout << std::string(60, '=') << "\n" << std::endl;

    return 0;
}
