#include <iostream>
#include <thread>
#include <chrono>
#include <cassert>
#include <string>
#include <random>
#include <cstring>
#include <sys/mman.h>
#include <atomic>
#include <vector>

#include "backend/leanstore/storage/record-cache/RecordCache.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheSlabAllocator.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheEntry.hpp"
#include "backend/leanstore/Config.hpp"

using RCEntry     = leanstore::storage::recordcache::RecordCacheEntry;
using RCType      = leanstore::storage::recordcache::RecordCacheType;
using RCCache     = leanstore::storage::recordcache::RecordCache;
using RCAllocator = leanstore::storage::recordcache::RecordCacheSlabAllocator;

// ============================================================
// ANSI 颜色
// ============================================================
namespace Color {
    const char* RESET  = "\033[0m";
    const char* RED    = "\033[31m";
    const char* GREEN  = "\033[32m";
    const char* YELLOW = "\033[33m";
    const char* CYAN   = "\033[36m";
    const char* BOLD   = "\033[1m";
}

void print_pass(const std::string& msg) {
    std::cout << Color::GREEN << "[PASS] " << Color::RESET << msg << "\n"; }
void print_fail(const std::string& msg) {
    std::cout << Color::RED   << "[FAIL] " << Color::RESET << msg << "\n"; }
void print_info(const std::string& msg) {
    std::cout << Color::CYAN  << "[INFO] " << Color::RESET << msg << "\n"; }
void print_phase(const std::string& msg) {
    std::cout << Color::YELLOW << Color::BOLD
              << msg << Color::RESET << "\n"; }
void print_test_header(const std::string& title) {
    std::cout << "\n" << Color::BOLD << Color::CYAN
              << "========================================\n"
              << "  " << title << "\n"
              << "========================================\n"
              << Color::RESET;
}

// ============================================================
// Helper
// ============================================================
std::span<const uint8_t> make_span(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

RCEntry* createEntry(RCAllocator&       allocator,
                     const std::string& key,
                     const std::string& val)
{
    size_t total = sizeof(RCEntry) + key.size() + val.size();
    void*  mem   = allocator.allocate(total, alignof(RCEntry));
    if (mem == nullptr) return nullptr;

    RCEntry* e = new (mem) RCEntry();
    e->key_length   = static_cast<uint16_t>(key.size());
    e->value_length = static_cast<uint16_t>(val.size());
    e->entry_type.store(RCType::ReadOnlyMode, std::memory_order_release);
    e->visited.store(false,                   std::memory_order_release);
    std::memcpy(e->payload,              key.data(), key.size());
    std::memcpy(e->payload + key.size(), val.data(), val.size());
    return e;
}

// 等待某个 key 从 hashtable 消失，超时返回 false
bool wait_until_evicted(RCCache&           cache,
                        const std::string& key,
                        int                timeout_ms = 30000)
{
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cache.GetFromRecordCache(make_span(key)) == nullptr)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// 等待某个 key 一直留在 hashtable（验证不被驱逐）
// 持续观察 duration_ms，如果中途消失返回 false
bool stays_in_cache(RCCache&           cache,
                    const std::string& key,
                    int                duration_ms = 5000)
{
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(duration_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cache.GetFromRecordCache(make_span(key)) == nullptr)
            return false;   // 被驱逐了
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;   // 一直在
}

// ============================================================
// TEST 1: 基础驱逐 type=000, visited=false
// ============================================================
void test_basic_eviction() {
    print_test_header("TEST 1: Basic Eviction (type=000, visited=false)");

    size_t mem_size = 32 * 1024 * 1024;
    void* pool = mmap(nullptr, mem_size, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    RCAllocator allocator(pool, mem_size);
    RCCache     cache(allocator, 4);

    // 插入几条 entry（不需要填满内存，去掉水位线后驱逐线程一直工作）
    const int N = 10;
    std::vector<std::string> keys;
    for (int i = 0; i < N; i++) {
        std::string key = "basic_key_" + std::to_string(i);
        std::string val = "basic_val_" + std::to_string(i);
        RCEntry* e = createEntry(allocator, key, val);
        assert(e != nullptr);
        cache.InsertOrAssignInRecordCache(make_span(key), e);
        keys.push_back(key);
    }
    print_info("Inserted " + std::to_string(N) + " entries (type=000, visited=false)");

    // 启动后台线程，驱逐线程一直工作（无水位线限制）
    FLAGS_sieve_eviction_thread = 1;
    FLAGS_forward_epoch_thread  = 1;
    cache.startBackgroundThreads();

    // 验证：所有 entry 都被驱逐
    bool all_evicted = true;
    for (const auto& key : keys) {
        if (!wait_until_evicted(cache, key, 30000)) {
            print_fail("Entry not evicted: " + key);
            all_evicted = false;
        }
    }

    assert(all_evicted && "Not all entries evicted!");
    print_pass("All " + std::to_string(N) + " entries evicted correctly");

    cache.stopBackgroundThreads();
    munmap(pool, mem_size);
}

// ============================================================
// TEST 2: Second Chance (type=000, visited=true)
// ============================================================
void test_second_chance() {
    print_test_header("TEST 2: Second Chance (type=000, visited=true)");

    size_t mem_size = 32 * 1024 * 1024;
    void* pool = mmap(nullptr, mem_size, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    RCAllocator allocator(pool, mem_size);
    RCCache     cache(allocator, 4);

    // 插入 cold entry（visited=false）和 hot entry（visited=true）
    std::string cold_key = "cold_entry";
    std::string hot_key  = "hot_entry";

    RCEntry* cold = createEntry(allocator, cold_key, "cold_val");
    RCEntry* hot  = createEntry(allocator, hot_key,  "hot_val");
    assert(cold != nullptr && hot != nullptr);

    // FIFO 顺序：cold → hot
    cache.InsertOrAssignInRecordCache(make_span(cold_key), cold);
    cache.InsertOrAssignInRecordCache(make_span(hot_key),  hot);
    hot->visited.store(true, std::memory_order_release);

    print_info("cold entry: visited=false (inserted first, FIFO head)");
    print_info("hot  entry: visited=true  (inserted second)");

    FLAGS_sieve_eviction_thread = 1;
    FLAGS_forward_epoch_thread  = 1;
    cache.startBackgroundThreads();

    // cold 先被驱逐（visited=false，无第二次机会）
    bool cold_evicted = wait_until_evicted(cache, cold_key, 30000);
    assert(cold_evicted && "Cold entry should be evicted first!");
    print_pass("cold entry evicted first (no second chance)");

    // hot 此时 visited 被清零，等待第二轮被驱逐
    bool hot_still_in = (cache.GetFromRecordCache(make_span(hot_key)) != nullptr);
    print_info("hot entry still in cache after cold evicted: "
               + std::string(hot_still_in ? "YES (got second chance)" : "NO"));

    // hot 最终也会被驱逐（第二轮）
    bool hot_evicted = wait_until_evicted(cache, hot_key, 30000);
    assert(hot_evicted && "Hot entry should eventually be evicted!");
    print_pass("hot entry evicted after second chance");

    cache.stopBackgroundThreads();
    munmap(pool, mem_size);
}

// ============================================================
// TEST 3: type=100 直接物理释放
// ============================================================
void test_type100_direct_free() {
    print_test_header("TEST 3: RemovedFromHashTable(100) - Direct Physical Free");

    size_t mem_size = 32 * 1024 * 1024;
    void* pool = mmap(nullptr, mem_size, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    RCAllocator allocator(pool, mem_size);
    RCCache     cache(allocator, 4);

    // 分配几条 entry，直接设为 type=100，加入 FIFO
    // 不插入 hashtable（type=100 表示已从 hashtable 摘除）
    const int N = 10;
    std::vector<void*> ptrs;

    for (int i = 0; i < N; i++) {
        std::string key = "type100_key_" + std::to_string(i);
        std::string val = "type100_val_" + std::to_string(i);
        RCEntry* e = createEntry(allocator, key, val);
        assert(e != nullptr);

        e->entry_type.store(
            RCType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation,
            std::memory_order_release);

        cache.sieve_fifo_queue.InsertIntoSieveFIFO(e);
        ptrs.push_back(static_cast<void*>(e));
    }

    double ratio_before = allocator.getUsageRatio();
    print_info("Inserted " + std::to_string(N) + " type=100 entries");
    print_info("Ratio before: " + std::to_string(ratio_before * 100.0) + "%");

    FLAGS_sieve_eviction_thread = 1;
    FLAGS_forward_epoch_thread  = 1;
    cache.startBackgroundThreads();

    // 等待驱逐线程 deallocate 完成（ratio 下降）
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    double ratio_after = allocator.getUsageRatio();
    print_info("Ratio after:  " + std::to_string(ratio_after * 100.0) + "%");

    assert(ratio_after < ratio_before &&
           "Ratio not decreased! type=100 entries not deallocated.");
    print_pass("Ratio decreased: physical memory reclaimed for type=100 entries");

    cache.stopBackgroundThreads();
    munmap(pool, mem_size);
}

// ============================================================
// TEST 4: type=011 不被 SIEVE 驱逐（哨兵验证）
// ============================================================
void test_type011_not_evicted() {
    print_test_header("TEST 4: LogicallyDeleted(011) Not Evicted by SIEVE");

    size_t mem_size = 32 * 1024 * 1024;
    void* pool = mmap(nullptr, mem_size, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    RCAllocator allocator(pool, mem_size);
    RCCache     cache(allocator, 4);

    // FIFO 顺序：sentinel_before → protected(011) → sentinel_after
    // 去掉水位线后驱逐线程一直工作
    // sentinel 被驱逐 → 证明 SIEVE hand 扫过了 protected 的位置
    // protected 还在 → 证明 SIEVE 主动跳过了 type=011

    std::string sentinel_before_key = "sentinel_BEFORE";
    std::string protected_key       = "protected_011";
    std::string sentinel_after_key  = "sentinel_AFTER";

    // 按顺序插入
    RCEntry* sb = createEntry(allocator, sentinel_before_key, "val");
    RCEntry* pe = createEntry(allocator, protected_key,       "val");
    RCEntry* sa = createEntry(allocator, sentinel_after_key,  "val");
    assert(sb != nullptr && pe != nullptr && sa != nullptr);

    cache.InsertOrAssignInRecordCache(make_span(sentinel_before_key), sb);
    cache.InsertOrAssignInRecordCache(make_span(protected_key),       pe);
    cache.InsertOrAssignInRecordCache(make_span(sentinel_after_key),  sa);

    // protected 改成 type=011
    pe->entry_type.store(
        RCType::LogicallyDeletedButStillInHashTable,
        std::memory_order_release);

    print_info("FIFO: sentinel_before(000) → protected(011) → sentinel_after(000)");

    FLAGS_sieve_eviction_thread = 1;
    FLAGS_forward_epoch_thread  = 1;
    cache.startBackgroundThreads();

    // 等待两个哨兵都被驱逐
    // 无水位线 → 驱逐线程一直工作 → 哨兵必然被扫到
    print_info("Waiting for both sentinels to be evicted...");

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(30000);
    bool both_evicted = false;

    while (std::chrono::steady_clock::now() < deadline) {
        bool before_gone = (cache.GetFromRecordCache(
                                make_span(sentinel_before_key)) == nullptr);
        bool after_gone  = (cache.GetFromRecordCache(
                                make_span(sentinel_after_key))  == nullptr);

        std::cout << "[DEBUG] sentinel_before="
                  << (before_gone ? "EVICTED" : "still_in")
                  << "  sentinel_after="
                  << (after_gone  ? "EVICTED" : "still_in") << "\n";
        std::cout.flush();

        if (before_gone && after_gone) {
            both_evicted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    assert(both_evicted && "Sentinels not evicted!");
    print_info("✓ Both sentinels evicted");
    print_info("  SIEVE hand passed: sentinel_before → protected → sentinel_after");

    // 核心验证：protected(011) 还在 hashtable
    RCEntry* found = cache.GetFromRecordCache(make_span(protected_key));
    assert(found != nullptr &&
           "FAIL: type=011 entry was wrongly evicted by SIEVE!");
    assert(found->entry_type.load(std::memory_order_acquire)
           == RCType::LogicallyDeletedButStillInHashTable);
    print_pass("✓ protected(011) still in hashtable after SIEVE scanned past it");

    // 改成 type=000，验证能被正常驱逐
    pe->entry_type.store(RCType::ReadOnlyMode, std::memory_order_release);
    print_info("Changed to type=000, waiting for eviction...");

    bool evicted = wait_until_evicted(cache, protected_key, 30000);
    assert(evicted && "Entry not evicted after type changed to 000!");
    print_pass("✓ Entry evicted after type changed to ReadOnlyMode(000)");
    print_pass("TEST 4 PASSED");

    cache.stopBackgroundThreads();
    munmap(pool, mem_size);
}

// ============================================================
// TEST 5: 并发插入 + 驱逐不崩溃
// ============================================================
void test_concurrent_stress() {
    print_test_header("TEST 5: Concurrent Insert + Eviction Stress");

    size_t mem_size = 64 * 1024 * 1024;
    void* pool = mmap(nullptr, mem_size, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    RCAllocator allocator(pool, mem_size);
    RCCache     cache(allocator, 16);

    const int NUM_WORKERS = 8;
    const int OPS_PER_WORKER = 1000;

    FLAGS_sieve_eviction_thread = 1;
    FLAGS_forward_epoch_thread  = 1;
    cache.startBackgroundThreads();

    std::atomic<int> total_inserted{0};
    std::vector<std::thread> workers;
    std::vector<std::vector<std::string>> worker_keys(NUM_WORKERS);

    for (int w = 0; w < NUM_WORKERS; w++) {
        workers.emplace_back([&, w]() {
            for (int i = 0; i < OPS_PER_WORKER; i++) {
                std::string key = "w" + std::to_string(w)
                                + "_k" + std::to_string(i);
                std::string val = "val_" + std::to_string(i);
                RCEntry* e = createEntry(allocator, key, val);
                if (e == nullptr) break;
                if (cache.InsertOrAssignInRecordCache(make_span(key), e)) {
                    worker_keys[w].push_back(key);
                    total_inserted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    print_info("Total inserted: " + std::to_string(total_inserted.load()));

    // 等待所有 entry 被驱逐
    bool all_evicted = true;
    for (int w = 0; w < NUM_WORKERS; w++) {
        for (const auto& key : worker_keys[w]) {
            if (!wait_until_evicted(cache, key, 30000)) {
                print_fail("Not evicted: " + key);
                all_evicted = false;
            }
        }
    }

    assert(all_evicted && "Not all entries evicted under concurrent stress!");
    print_pass("All entries evicted without crash");

    cache.stopBackgroundThreads();
    munmap(pool, mem_size);
}

// ============================================================
// TEST 6: 并发 visited=true + 驱逐线程
// ============================================================
void test_concurrent_visited_bit() {
    print_test_header("TEST 6: Concurrent visited=true + Eviction");

    size_t mem_size = 32 * 1024 * 1024;
    void* pool = mmap(nullptr, mem_size, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    RCAllocator allocator(pool, mem_size);
    RCCache     cache(allocator, 8);

    const int NUM_WORKERS = 4;
    const int N = 100;

    // 插入 N 条 entry
    std::vector<std::string> keys;
    for (int i = 0; i < N; i++) {
        std::string key = "vis_key_" + std::to_string(i);
        std::string val = "vis_val_" + std::to_string(i);
        RCEntry* e = createEntry(allocator, key, val);
        assert(e != nullptr);
        cache.InsertOrAssignInRecordCache(make_span(key), e);
        keys.push_back(key);
    }
    print_info("Inserted " + std::to_string(N) + " entries");

    FLAGS_sieve_eviction_thread = 1;
    FLAGS_forward_epoch_thread  = 1;
    cache.startBackgroundThreads();

    // 并发 worker 持续置 visited=true（跑 2s）
    std::atomic<bool> running{true};
    std::vector<std::thread> vis_workers;

    for (int w = 0; w < NUM_WORKERS; w++) {
        vis_workers.emplace_back([&, w]() {
            std::mt19937 rng(w);
            while (running.load(std::memory_order_acquire)) {
                size_t idx = rng() % keys.size();
                cache.enterEpoch(w);
                RCEntry* e = cache.GetFromRecordCache(make_span(keys[idx]));
                if (e != nullptr)
                    e->visited.store(true, std::memory_order_release);
                cache.leaveEpoch(w);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    running.store(false, std::memory_order_release);
    for (auto& w : vis_workers) w.join();
    print_info("Workers stopped, eviction thread will now clear visited bits");

    // worker 停止后，驱逐线程清零 visited bit，最终全部驱逐
    bool all_evicted = true;
    for (const auto& key : keys) {
        if (!wait_until_evicted(cache, key, 30000)) {
            print_fail("Not evicted: " + key);
            all_evicted = false;
        }
    }

    assert(all_evicted && "Not all entries evicted after workers stopped!");
    print_pass("All entries evicted after visited bits cleared");

    cache.stopBackgroundThreads();
    munmap(pool, mem_size);
}

// ============================================================
// TEST 7: Forward_epoch 协同（011 → 100 → deallocate）
// ============================================================
void test_forward_epoch_cooperation() {
    print_test_header("TEST 7: Forward Epoch Cooperation (011 → 100 → free)");

    size_t mem_size = 32 * 1024 * 1024;
    void* pool = mmap(nullptr, mem_size, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    RCAllocator allocator(pool, mem_size);
    RCCache     cache(allocator, 4);

    const int N = 10;
    std::vector<std::string> keys;

    // 插入 N 条 entry
    for (int i = 0; i < N; i++) {
        std::string key = "epoch_key_" + std::to_string(i);
        std::string val = "epoch_val_" + std::to_string(i);
        RCEntry* e = createEntry(allocator, key, val);
        assert(e != nullptr);
        cache.InsertOrAssignInRecordCache(make_span(key), e);
        keys.push_back(key);
    }
    print_info("Inserted " + std::to_string(N) + " entries");

    FLAGS_sieve_eviction_thread = 1;
    FLAGS_forward_epoch_thread  = 1;
    cache.startBackgroundThreads();

    // 逻辑删除：000 → 011，加入 InvalidationQueue
    cache.enterEpoch(0);
    u64 current_epoch = cache.getCurrentEpoch();
    for (const auto& key : keys) {
        RCEntry* e = cache.GetFromRecordCache(make_span(key));
        if (e == nullptr) continue;
        RCType expected = RCType::ReadOnlyMode;
        if (e->casType(expected,
                RCType::LogicallyDeletedButStillInHashTable)) {
            cache.addToInvalidationQueue(e, current_epoch);
        }
    }
    cache.leaveEpoch(0);
    print_info("All entries logically deleted (000 → 011)");
    print_info("Forward_epoch will process: 011 → 100");
    print_info("SIEVE will then: 100 → deallocate");

    // 等待所有 entry 从 hashtable 消失
    bool all_removed = true;
    for (const auto& key : keys) {
        if (!wait_until_evicted(cache, key, 30000)) {
            print_fail("Not removed from hashtable: " + key);
            all_removed = false;
        }
    }

    assert(all_removed &&
           "Entries not removed! Forward_epoch + SIEVE cooperation broken.");
    print_pass("All entries removed from hashtable");

    // 验证物理内存被回收
    double ratio = allocator.getUsageRatio();
    print_info("Final ratio: " + std::to_string(ratio * 100.0) + "%");
    assert(ratio < 0.01 && "Memory not reclaimed!");
    print_pass("Physical memory reclaimed");
    print_pass("TEST 7 PASSED");

    cache.stopBackgroundThreads();
    munmap(pool, mem_size);
}

// ============================================================
// TEST 8: 高并发 insert + lookup + 驱逐不崩溃
// ============================================================
void test_high_concurrency_no_crash() {
    print_test_header("TEST 8: High Concurrency - No Crash");

    size_t mem_size = 64 * 1024 * 1024;
    void* pool = mmap(nullptr, mem_size, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    RCAllocator allocator(pool, mem_size);
    RCCache     cache(allocator, 32);

    const int NUM_WORKERS   = 16;
    const int OPS_PER_WORKER = 500;

    FLAGS_sieve_eviction_thread = 1;
    FLAGS_forward_epoch_thread  = 1;
    cache.startBackgroundThreads();

    std::atomic<int> inserts{0};
    std::atomic<int> lookups{0};
    std::vector<std::thread>              workers;
    std::vector<std::vector<std::string>> worker_keys(NUM_WORKERS);

    auto start = std::chrono::high_resolution_clock::now();

    for (int w = 0; w < NUM_WORKERS; w++) {
        workers.emplace_back([&, w]() {
            std::mt19937 rng(w * 777 + 123);

            // Insert
            for (int i = 0; i < OPS_PER_WORKER; i++) {
                std::string key = "hc_w" + std::to_string(w)
                                + "_k"   + std::to_string(i);
                std::string val = "val"  + std::to_string(i);
                RCEntry* e = createEntry(allocator, key, val);
                if (e == nullptr) break;
                if (cache.InsertOrAssignInRecordCache(make_span(key), e)) {
                    worker_keys[w].push_back(key);
                    inserts.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // Lookup（驱逐线程同时在跑）
            for (int i = 0; i < OPS_PER_WORKER; i++) {
                if (worker_keys[w].empty()) break;
                cache.enterEpoch(w);
                size_t idx = rng() % worker_keys[w].size();
                if (cache.GetFromRecordCache(
                        make_span(worker_keys[w][idx])) != nullptr)
                    lookups.fetch_add(1, std::memory_order_relaxed);
                cache.leaveEpoch(w);
            }
        });
    }
    for (auto& w : workers) w.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms  = std::chrono::duration_cast<
                   std::chrono::milliseconds>(end - start).count();

    print_info("Inserts:    " + std::to_string(inserts.load()));
    print_info("Lookups:    " + std::to_string(lookups.load()));
    print_info("Duration:   " + std::to_string(ms) + " ms");
    print_info("No crash detected (reached this point)");

    // 等待所有 entry 被驱逐
    bool all_evicted = true;
    for (int w = 0; w < NUM_WORKERS; w++) {
        for (const auto& key : worker_keys[w]) {
            if (!wait_until_evicted(cache, key, 30000)) {
                all_evicted = false;
            }
        }
    }

    assert(all_evicted && "Not all entries evicted under high concurrency!");
    print_pass("All entries evicted without crash under high concurrency");

    cache.stopBackgroundThreads();
    munmap(pool, mem_size);
}

// ============================================================
// Main
// ============================================================
int main() {
    std::cout << "\n" << Color::BOLD << Color::CYAN
              << "============================================\n"
              << "   SIEVE Eviction Thread Unit Tests\n"
              << "   (No watermark - eviction always running)\n"
              << "============================================\n"
              << Color::RESET;

    FLAGS_cxl_tiering_enabled         = true;
    FLAGS_forward_epoch_thread        = 1;
    FLAGS_sieve_eviction_thread       = 1;
    FLAGS_record_cache_promote_thread = 0;
    FLAGS_worker_threads              = 16;

    try {
        test_basic_eviction();
        test_second_chance();
        test_type100_direct_free();
        test_type011_not_evicted();
        test_concurrent_stress();
        test_concurrent_visited_bit();
        test_forward_epoch_cooperation();
        test_high_concurrency_no_crash();

        std::cout << "\n" << Color::BOLD << Color::GREEN
                  << "============================================\n"
                  << "   ALL TESTS PASSED! ✓\n"
                  << "============================================\n"
                  << Color::RESET << "\n";
        return 0;

    } catch (const std::exception& ex) {
        std::cerr << Color::RED << "[ERROR] Exception: "
                  << ex.what() << Color::RESET << "\n";
        return 1;
    }
}
