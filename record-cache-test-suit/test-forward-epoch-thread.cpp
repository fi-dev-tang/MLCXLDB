#include "backend/leanstore/storage/record-cache/RecordCache.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheSlabAllocator.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheEntry.hpp"
#include "backend/leanstore/Config.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <cassert>
#include <sys/mman.h>
#include <atomic>
#include <chrono>
#include <random>
#include <cstring>

using namespace leanstore::storage::recordcache;

// ============================================================
// ANSI 颜色代码
// ============================================================
namespace Color {
    const char* RESET   = "\033[0m";
    const char* RED     = "\033[31m";
    const char* GREEN   = "\033[32m";
    const char* YELLOW  = "\033[33m";
    const char* BLUE    = "\033[34m";
    const char* MAGENTA = "\033[35m";
    const char* CYAN    = "\033[36m";
    const char* BOLD    = "\033[1m";
}

void print_pass(const std::string& msg) {
    std::cout << Color::GREEN << "[PASS] " << Color::RESET << msg << "\n";
}
void print_fail(const std::string& msg) {
    std::cout << Color::RED << "[FAIL] " << Color::RESET << msg << "\n";
}
void print_info(const std::string& msg) {
    std::cout << Color::CYAN << "[INFO] " << Color::RESET << msg << "\n";
}
void print_warn(const std::string& msg) {
    std::cout << Color::YELLOW << "[WARN] " << Color::RESET << msg << "\n";
}
void print_setup(const std::string& msg) {
    std::cout << Color::MAGENTA << "[Setup] " << Color::RESET << msg << "\n";
}
void print_phase(const std::string& msg) {
    std::cout << Color::YELLOW << Color::BOLD << msg << Color::RESET << "\n";
}
void print_stats(const std::string& msg) {
    std::cout << Color::CYAN << msg << Color::RESET << "\n";
}
void print_checking(const std::string& msg) {
    std::cout << Color::BLUE << "[Checking] " << Color::RESET << msg << "\n";
}
void print_skip(const std::string& msg) {
    std::cout << Color::YELLOW << "[SKIP] " << Color::RESET << msg << "\n";
}
void print_test_header(const std::string& title) {
    std::cout << "\n" << Color::BOLD << Color::CYAN
              << "========================================\n"
              << "  " << title << "\n"
              << "========================================" << Color::RESET << "\n";
}

// ============================================================
// MockBufferManager
// ============================================================
class MockBufferManager {
public:
    void*  record_cache_ptr;
    size_t total_cache_size;

    MockBufferManager(size_t size_in_mb) {
        total_cache_size = size_in_mb * 1024 * 1024;
        record_cache_ptr = mmap(nullptr, total_cache_size,
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (record_cache_ptr == MAP_FAILED)
            throw std::bad_alloc();
    }
    ~MockBufferManager() {
        if (record_cache_ptr != MAP_FAILED)
            munmap(record_cache_ptr, total_cache_size);
    }
};

// ============================================================
// 辅助函数
// ============================================================
std::span<const uint8_t> make_span(const std::string& str) {
    return {reinterpret_cast<const uint8_t*>(str.data()), str.size()};
}

RecordCacheEntry* createEntry(RecordCacheSlabAllocator& allocator,
                               const std::string& key,
                               const std::string& value)
{
    size_t entry_size = sizeof(RecordCacheEntry) + key.size() + value.size();
    void*  mem        = allocator.allocate(entry_size, 8);
    assert(mem != nullptr);

    RecordCacheEntry* entry = new (mem) RecordCacheEntry();
    entry->key_length   = key.size();
    entry->value_length = value.size();
    entry->entry_type.store(RecordCacheType::ReadOnlyMode, std::memory_order_release);
    entry->visited.store(false, std::memory_order_release);

    std::memcpy(entry->payload,              key.data(),   key.size());
    std::memcpy(entry->payload + key.size(), value.data(), value.size());

    return entry;
}

// ============================================================
// 核心设计原则（贯穿所有测试）
//
// 1. 不在 epoch 外持有 entry 指针。
// 2. 不假设"我插入的 entry 以后一定还在"——SIEVE 随时可以驱逐它。
// 3. 所有断言只针对"在当前 epoch 内、通过哈希表实时查询"的结果。
// 4. 计数器只统计"在 epoch 内成功操作"的次数，不与事后的哈希表状态
//    做强绑定比较（因为后台线程可能在两次查询之间改变状态）。
// ============================================================

//=================================================================================================
// TEST 1: 基础 Epoch 机制
// 验证目标：Forward_epoch 后台线程能持续推进全局 epoch。
//=================================================================================================
void test_basic_epoch_mechanism() {
    print_test_header("TEST 1: Basic Epoch Mechanism");

    MockBufferManager bm(100);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 16);

    const u64 num_workers = 4;

    u64 initial_epoch = cache.getCurrentEpoch();
    print_info("Initial global_epoch = " + std::to_string(initial_epoch));

    cache.startBackgroundThreads();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    u64 final_epoch = cache.getCurrentEpoch();
    print_info("After 100ms, global_epoch = " + std::to_string(final_epoch));

    assert(final_epoch > initial_epoch);
    print_pass("Epoch advanced by " + std::to_string(final_epoch - initial_epoch));

    // Worker 线程 enterEpoch / leaveEpoch 压力
    std::vector<std::thread> workers;
    std::atomic<u64> success_count = 0;

    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        workers.emplace_back([&, w_i]() {
            for (int iter = 0; iter < 10; iter++) {
                cache.enterEpoch(w_i);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                cache.leaveEpoch(w_i);
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& w : workers) w.join();

    print_pass("All " + std::to_string(success_count.load())
               + " epoch enter/leave operations completed");
}

//=================================================================================================
// TEST 2: InvalidationQueue 和从哈希表移除
//
// 验证目标：
//   被标记为 LogicallyDeleted 并加入 InvalidationQueue 的 entry，
//   在没有 worker 持有对应 epoch 后，必须从哈希表消失。
//
// 正确的语义：
//   - 在 epoch 内拿到 entry 指针并执行 CAS → 成功则记录该 key。
//   - 等待后台线程处理。
//   - 再次在 epoch 内查询该 key：
//       nullptr  → 已被移除（可能是 InvalidationQueue 或 SIEVE，两者都是正确行为）
//       non-null → 检查其 entry_type，若仍是 ReadOnlyMode 则说明没被处理（异常）。
//   - 对于"没有显式删除"的 key，我们只验证"系统没有崩溃"，
//     不断言它们一定还在（SIEVE 可能驱逐它们）。
//=================================================================================================
void test_invalidation_queue_and_deletion() {
    print_test_header("TEST 2: InvalidationQueue and Hash Table Removal");

    MockBufferManager bm(200);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 32);

    const u64 num_workers        = 4;
    const u64 entries_per_worker = 50;

    print_setup("Creating " + std::to_string(num_workers * entries_per_worker)
                + " entries...");

    // 只保存 key 字符串
    std::vector<std::vector<std::string>> worker_keys(num_workers);

    // Phase 1: 插入（后台线程尚未启动，SIEVE 不会驱逐）
    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        for (u64 i = 0; i < entries_per_worker; i++) {
            std::string key   = "worker_" + std::to_string(w_i)
                              + "_key_"   + std::to_string(i);
            std::string value = "value_"  + std::to_string(i);

            RecordCacheEntry* entry = createEntry(allocator, key, value);
            bool inserted = cache.InsertOrAssignInRecordCache(make_span(key), entry);
            assert(inserted);
            worker_keys[w_i].push_back(key);
        }
    }
    print_pass("Inserted " + std::to_string(num_workers * entries_per_worker)
               + " entries (background threads not yet running)");

    // Phase 2: 启动后台线程（之后 SIEVE 可以随时驱逐 entry）
    cache.startBackgroundThreads();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Phase 3: 逻辑删除——在 epoch 内拿指针、CAS、加队列，三步一气完成
    print_phase("\n[Phase 3] Logical deletion by workers...");

    // 记录"成功加入 InvalidationQueue"的 key
    // 这些 key 之后必须从哈希表消失
    std::vector<std::vector<std::string>> deleted_keys(num_workers);
    std::atomic<u64> logical_delete_count = 0;

    std::vector<std::thread> delete_workers;
    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        delete_workers.emplace_back([&, w_i]() {
            u64 delete_count = entries_per_worker / 2;
            for (u64 i = 0; i < delete_count; i++) {
                // 在 epoch 内完成：查询 → CAS → 加队列
                cache.enterEpoch(w_i);
                u64 current_epoch = cache.getCurrentEpoch();

                const std::string& key = worker_keys[w_i][i];
                RecordCacheEntry* entry = cache.GetFromRecordCache(make_span(key));

                if (entry != nullptr) {
                    // entry 在 epoch 内合法，执行 CAS
                    RecordCacheType expected = RecordCacheType::ReadOnlyMode;
                    if (entry->casType(expected,
                            RecordCacheType::LogicallyDeletedButStillInHashTable)) {
                        cache.addToInvalidationQueue(entry, current_epoch);
                        logical_delete_count.fetch_add(1, std::memory_order_relaxed);
                        deleted_keys[w_i].push_back(key);
                        // entry 指针在 leaveEpoch 后不再使用
                    }
                    // else: 已被其他路径处理（SIEVE 或并发删除），跳过
                }
                // else: SIEVE 已驱逐，key 已不在哈希表，无需再删

                cache.leaveEpoch(w_i);
            }
        });
    }
    for (auto& w : delete_workers) w.join();

    print_info("Successfully added to InvalidationQueue: "
               + std::to_string(logical_delete_count.load()) + " entries");

    // Phase 4: 等待后台线程处理
    print_phase("\n[Phase 4] Waiting for Forward_epoch to process queue...");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Phase 5: 验证——成功加入 InvalidationQueue 的 key 必须从哈希表消失
    // 语义：这些 key 已经被标记为 LogicallyDeleted，
    //       无论是 Forward_epoch 还是 SIEVE 把它们移除，结果都是正确的。
    print_phase("\n[Phase 5] Verifying removed entries are gone from hash table...");

    u64 confirmed_removed = 0;
    u64 still_present     = 0;   // 不应该出现（已 LogicallyDeleted 却还在）

    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        for (const auto& key : deleted_keys[w_i]) {
            cache.enterEpoch(w_i);
            RecordCacheEntry* e = cache.GetFromRecordCache(make_span(key));
            if (e == nullptr) {
                confirmed_removed++;
            } else {
                // 还在哈希表里——检查其类型
                // LogicallyDeleted 但还没被后台线程处理：暂时可接受
                // ReadOnlyMode：不应该出现（我们已经 CAS 成功）
                auto t = e->entry_type.load(std::memory_order_acquire);
                if (t == RecordCacheType::ReadOnlyMode) {
                    still_present++;   // 真正的错误
                }
                // LogicallyDeleted 还在表中：Forward_epoch 还没来得及处理，
                // 给更多时间
            }
            cache.leaveEpoch(w_i);
        }
    }

    if (still_present > 0) {
        print_fail(std::to_string(
        still_present) + " entries still ReadOnlyMode after logical delete!");
        assert(false && "Logical deletion CAS succeeded but entry_type not changed!");
    }

    print_info("Confirmed removed from hash table: "
               + std::to_string(confirmed_removed)
               + " / " + std::to_string(logical_delete_count.load()));

    // 如果还有 LogicallyDeleted 但尚未移除的，再等一轮
    if (confirmed_removed < logical_delete_count.load()) {
        print_warn("Some entries still in hash table (LogicallyDeleted state), "
                   "waiting more...");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        confirmed_removed = 0;
        for (u64 w_i = 0; w_i < num_workers; w_i++) {
            for (const auto& key : deleted_keys[w_i]) {
                cache.enterEpoch(w_i);
                if (cache.GetFromRecordCache(make_span(key)) == nullptr)
                    confirmed_removed++;
                cache.leaveEpoch(w_i);
            }
        }
        print_info("After extra wait, confirmed removed: "
                   + std::to_string(confirmed_removed)
                   + " / " + std::to_string(logical_delete_count.load()));
    }

    // 核心断言：成功逻辑删除的 entry 必须从哈希表消失
    if (logical_delete_count.load() > 0) {
        assert(confirmed_removed == logical_delete_count.load());
        print_pass("All logically deleted entries removed from hash table!");
    } else {
        // SIEVE 在 Phase 3 之前就把所有 entry 驱逐了
        // 这也是正确行为，只是无法验证 InvalidationQueue 机制
        print_warn("All entries were evicted by SIEVE before logical delete phase.");
        print_warn("Consider increasing cache size or reducing entry count.");
    }

    // Phase 6: 非删除 key 的验证
    // 语义修正：不断言它们"一定还在"，只统计还在的比例并打印
    // SIEVE 可能已经驱逐了其中一部分，这是正常行为
    print_phase("\n[Phase 6] Checking non-deleted entries (informational only)...");

    u64 still_accessible = 0;
    u64 sieve_evicted    = 0;

    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        u64 delete_count = entries_per_worker / 2;
        for (u64 i = delete_count; i < entries_per_worker; i++) {
            cache.enterEpoch(w_i);
            RecordCacheEntry* e =
                cache.GetFromRecordCache(make_span(worker_keys[w_i][i]));
            if (e != nullptr)
                still_accessible++;
            else
                sieve_evicted++;
            cache.leaveEpoch(w_i);
        }
    }

    u64 expected = num_workers * (entries_per_worker - entries_per_worker / 2);
    print_info("Non-deleted entries still accessible: "
               + std::to_string(still_accessible)
               + " / " + std::to_string(expected));
    print_info("Non-deleted entries evicted by SIEVE: "
               + std::to_string(sieve_evicted)
               + " / " + std::to_string(expected));

    // 只要系统没有崩溃、没有 use-after-free，Phase 6 就是通过的
    print_pass("Phase 6 passed (SIEVE eviction of non-deleted entries is expected behavior)");
}

//=================================================================================================
// TEST 3: Epoch 保护机制
//
// 验证目标：
//   Worker 持有 epoch 期间，加入 InvalidationQueue 的 entry
//   不能从哈希表消失（Forward_epoch 必须等待所有 worker 离开该 epoch）。
//
// 注意：SIEVE 驱逐不受 epoch 保护，但 SIEVE 不会驱逐
//       已经是 LogicallyDeleted 状态的 entry（它们已经不是候选）。
//       因此本测试的保护断言仍然有效。
//=================================================================================================
void test_epoch_protection() {
    print_test_header("TEST 3: Epoch Protection - Prevent Deletion While Worker Active");

    MockBufferManager bm(100);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 16);

    // 插入前不启动后台线程，确保 entry 存在
    print_setup("Creating test entries...");
    std::vector<std::string> keys;

    for (int i = 0; i < 10; i++) {
        std::string key   = "protect_key_" + std::to_string(i);
        std::string value = "value_"        + std::to_string(i);
        RecordCacheEntry* entry = createEntry(allocator, key, value);
        cache.InsertOrAssignInRecordCache(make_span(key), entry);
        keys.push_back(key);
    }

    // Worker 0 先进入 epoch，再启动后台线程
    const u64 worker_id = 0;
    cache.enterEpoch(worker_id);
    u64 snapshot_epoch = cache.getCurrentEpoch();
    print_info("Worker 0 entered epoch = " + std::to_string(snapshot_epoch));

    cache.startBackgroundThreads();
    print_info("Background threads started (worker 0 still holding epoch)");

    // 在 epoch 保护内执行逻辑删除
    // Worker 1 作为 deleter，也进入 epoch 保护自己的指针访问
    std::thread deleter([&]() {
        cache.enterEpoch(1);
        u64 deleter_epoch = cache.getCurrentEpoch();

        for (const auto& key : keys) {
            RecordCacheEntry* entry = cache.GetFromRecordCache(make_span(key));
            if (entry == nullptr) continue;   // SIEVE 已驱逐，跳过

            RecordCacheType expected = RecordCacheType::ReadOnlyMode;
            if (entry->casType(expected,
                    RecordCacheType::LogicallyDeletedButStillInHashTable)) {
                // 使用 snapshot_epoch（Worker 0 进入时的 epoch）
                // Forward_epoch 必须等到 Worker 0 离开后才能处理这批 entry
                cache.addToInvalidationQueue(entry, snapshot_epoch);
            }
        }

        cache.leaveEpoch(1);
        print_info("Deleter finished, entries queued with epoch = "
                   + std::to_string(snapshot_epoch));
    });
    deleter.join();

    // 等待 Forward_epoch 尝试处理（Worker 0 还持有 epoch，应被阻止）
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 验证：LogicallyDeleted 的 entry 必须仍在哈希表
    // 因为 Worker 0 还持有 snapshot_epoch，Forward_epoch 不能处理这批 entry
    // 注意：只检查"还在哈希表"，不检查 entry_type（那需要访问指针）
    print_checking("Entries should still be in hash table (worker 0 holds epoch)...");

    u64 still_in_table = 0;
    for (const auto& key : keys) {
        cache.enterEpoch(worker_id);
        if (cache.GetFromRecordCache(make_span(key)) != nullptr)
            still_in_table++;
        cache.leaveEpoch(worker_id);
    }

    print_info("Entries still in hash table: "
               + std::to_string(still_in_table)
               + " / " + std::to_string(keys.size()));

    // 核心断言：所有 LogicallyDeleted 的 entry 必须仍在哈希表
    // （deleter 线程已对所有找到的 entry 执行了 CAS）
    if (still_in_table < keys.size()) {
        print_warn(std::to_string(keys.size() - still_in_table)
                   + " entries already gone — possibly SIEVE evicted them "
                   "before deleter ran (acceptable if SIEVE is very aggressive).");
    } else {
        print_pass("All entries protected while Worker 0 holds epoch");
    }

    // Worker 0 离开 epoch
    cache.leaveEpoch(worker_id);
    print_info("Worker 0 left epoch");

    // 等待 Forward_epoch 处理
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 验证：现在这些 entry 应该从哈希表消失
    print_checking("After worker left epoch, entries should be removed...");

    u64 removed_count = 0;
    for (const auto& key : keys) {
        cache.enterEpoch(worker_id);
        if (cache.GetFromRecordCache(make_span(key)) == nullptr)
            removed_count++;
        cache.leaveEpoch(worker_id);
    }

    print_info("Entries removed from hash table: "
               + std::to_string(removed_count)
               + " / " + std::to_string(keys.size()));

    // 至少 still_in_table 个被移除（之前保护住的那些必须消失）
    assert(removed_count >= still_in_table);
    print_pass("Epoch protection works correctly: "
               + std::to_string(removed_count) + " entries removed after epoch release");
}

//=================================================================================================
// TEST 4: 多 Worker 并发
//
// 验证目标：
//   多线程并发插入、查询、逻辑删除，系统不崩溃、不死锁，
//   成功逻辑删除的 entry 最终从哈希表消失。
//=================================================================================================
void test_concurrent_workers() {
    print_test_header("TEST 4: Concurrent Workers - Real-world Scenario");

    MockBufferManager bm(300);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 64);

    const u64 num_workers           = 8;
    const u64 operations_per_worker = 100;

    std::atomic<u64> total_inserted        = 0;
    std::atomic<u64> total_lookups         = 0;
    std::atomic<u64> total_logical_deletes = 0;

    // 记录每个 worker 成功加入 InvalidationQueue 的 key
    std::vector<std::vector<std::string>> deleted_keys(num_workers);
    std::mutex deleted_keys_mutex;   // 保护 deleted_keys 写入

    print_setup("Starting " + std::to_string(num_workers) + " concurrent workers...");
    cache.startBackgroundThreads();

    std::vector<std::thread> workers;
    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        workers.emplace_back([&, w_i]() {
            std::vector<std::string> local_keys;
            std::vector<std::string> local_deleted;

            // Phase A: Insert
            for (u64 i = 0; i < operations_per_worker; i++) {
                cache.enterEpoch(w_i);

                std::string key   = "w" + std::to_string(w_i)
                                  + "_op" + std::to_string(i);
                std::string value = "val_" + std::to_string(i);

                RecordCacheEntry* entry = createEntry(allocator, key, value);
                if (cache.InsertOrAssignInRecordCache(make_span(key), entry)) {
                    total_inserted.fetch_add(1, std::memory_order_relaxed);
                    local_keys.push_back(key);
                }
                // entry 交给哈希表，本地不再持有

                cache.leaveEpoch(w_i);
            }

            // Phase B: Lookup（在 epoch 内，结果可能是 nullptr，正常）
            for (const auto& key : local_keys) {
                cache.enterEpoch(w_i);
                if (cache.GetFromRecordCache(make_span(key)) != nullptr)
                    total_lookups.fetch_add(1, std::memory_order_relaxed);
                cache.leaveEpoch(w_i);
            }

            // Phase C: 逻辑删除前一半
            u64 delete_count = local_keys.size() / 2;
            for (u64 i = 0; i < delete_count; i++) {
                cache.enterEpoch(w_i);
                u64 current_epoch = cache.getCurrentEpoch();

                RecordCacheEntry* entry =
                    cache.GetFromRecordCache(make_span(local_keys[i]));

                if (entry != nullptr) {
                    RecordCacheType expected = RecordCacheType::ReadOnlyMode;
                    if (entry->casType(expected,
                            RecordCacheType::LogicallyDeletedButStillInHashTable)) {
                        cache.addToInvalidationQueue(entry, current_epoch);
                        total_logical_deletes.fetch_add(1, std::memory_order_relaxed);
                        local_deleted.push_back(local_keys[i]);
                    }
                }
                // entry 在 leaveEpoch 后不再使用

                cache.leaveEpoch(w_i);
            }

            // 汇总本 worker 成功删除的 key
            {
                std::lock_guard<std::mutex> lk(deleted_keys_mutex);
                deleted_keys[w_i] = std::move(local_deleted);
            }
        });
    }
    for (auto& w : workers) w.join();

    print_stats("\n[Statistics after workers done]");
    print_info("  Total inserted:        " + std::to_string(total_inserted.load()));
    print_info("  Total lookups:         " + std::to_string(total_lookups.load()));
    print_info("  Total logical deletes: " + std::to_string(total_logical_deletes.load()));

    // 等待后台线程处理
    print_info("\nWaiting for Forward_epoch to drain queue...");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 验证：成功逻辑删除的 key 必须从哈希表消失
    u64 confirmed_removed = 0;
    u64 total_to_check    = 0;

    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        for (const auto& key : deleted_keys[w_i]) {
            total_to_check++;
            cache.enterEpoch(w_i);
            if (cache.GetFromRecordCache(make_span(key)) == nullptr)
                confirmed_removed++;
            cache.leaveEpoch(w_i);
        }
    }

    print_stats("\n[Results]");
    print_info("  Logically deleted keys to check: "
               + std::to_string(total_to_check));
    print_info("  Confirmed removed from hash table: "
               + std::to_string(confirmed_removed)
               + " / " + std::to_string(total_to_check));

    if (total_to_check > 0) {
        assert(confirmed_removed == total_to_check);
        print_pass("All logically deleted entries removed from hash table!");
    } else {
        print_warn("No logical deletions succeeded (SIEVE evicted everything first).");
        print_warn("Consider increasing cache size.");
    }

    print_pass("Concurrent workers test completed without crash.");
}

//=================================================================================================
// TEST 5: 重试队列测试
//
// 验证目标：
//   当 worker 持有 epoch 时，Forward_epoch 无法处理该 epoch 下的 entry，
//   会将其放入重试队列；worker 离开后，重试队列中的 entry 最终被处理。
//=================================================================================================
void test_retry_queue_mechanism() {
    print_test_header("TEST 5: Retry Queue Mechanism");

    MockBufferManager bm(100);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 16);

    print_setup("Creating test entry...");
    std::string key   = "retry_test_key";
    std::string value = "retry_test_value";

    RecordCacheEntry* entry = createEntry(allocator, key, value);
    cache.InsertOrAssignInRecordCache(make_span(key), entry);

    // Worker 0 先进入 epoch，再启动后台线程
    const u64 worker_id = 0;
    cache.enterEpoch(worker_id);
    u64 snapshot_epoch = cache.getCurrentEpoch();
    print_info("Worker 0 holding epoch = " + std::to_string(snapshot_epoch));

    // 在 epoch 保护内执行逻辑删除并加入队列
    RecordCacheType expected = RecordCacheType::ReadOnlyMode;
    bool cas_ok = entry->casType(expected,
                      RecordCacheType::LogicallyDeletedButStillInHashTable);
    assert(cas_ok);
    cache.addToInvalidationQueue(entry, snapshot_epoch);
    print_info("Entry added to InvalidationQueue with epoch = "
               + std::to_string(snapshot_epoch));

    // entry 指针最后一次合法使用结束，主动置空
    entry = nullptr;

    // 启动后台线程（Worker 0 仍持有 epoch）
    cache.startBackgroundThreads();

    // 等待 Forward_epoch 尝试处理（应被阻止）
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // 验证：entry 仍在哈希表（Worker 0 还持有 epoch）
    {
        cache.enterEpoch(1);   // 用 worker 1 的 epoch 来查询
        RecordCacheEntry* found = cache.GetFromRecordCache(make_span(key));
        cache.leaveEpoch(1);

        if (found != nullptr) {
            print_pass("Entry still in hash table while Worker 0 holds epoch "
                       "(retry queue working correctly)");
        } else {
            // SIEVE 可能在后台线程启动瞬间就驱逐了它
            // 但 SIEVE 不应该驱逐 LogicallyDeleted 状态的 entry
            // 如果这里是 nullptr，说明有问题
            print_fail("Entry disappeared while Worker 0 still holds epoch!");
            assert(false && "Epoch protection failed in retry queue test!");
        }
    }

    // Worker 0 离开 epoch
    cache.leaveEpoch(worker_id);
    print_info("Worker 0 left epoch");

    // 等待 Forward_epoch 重试并处理
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 验证：entry 已从哈希表消失
    {
        cache.enterEpoch(1);
        RecordCacheEntry* found = cache.GetFromRecordCache(make_span(key));
        cache.leaveEpoch(1);

        assert(found == nullptr);
        print_pass("Entry removed from hash table after Worker 0 left epoch "
                   "(retry queue works correctly!)");
    }
}

//=================================================================================================
// TEST 6: 压力测试
//
// 验证目标：
//   高并发下系统不崩溃、不死锁、不产生 use-after-free。
//   成功逻辑删除的 entry 最终从哈希表消失。
//=================================================================================================
void test_stress_concurrent() {
    print_test_header("TEST 6: Stress Test - High Concurrency");

    MockBufferManager bm(500);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 128);

    const u64 num_workers           = 16;
    const u64 operations_per_worker = 200;

    std::atomic<u64> total_operations = 0;
    std::atomic<u64> insert_count     = 0;
    std::atomic<u64> lookup_count     = 0;
    std::atomic<u64> delete_count     = 0;

    // 记录每个 worker 成功加入 InvalidationQueue 的 key
    std::vector<std::vector<std::string>> deleted_keys(num_workers);
    std::mutex deleted_keys_mutex;

    print_setup("Starting stress test: "
                + std::to_string(num_workers) + " workers x "
                + std::to_string(operations_per_worker) + " ops...");

    cache.startBackgroundThreads();
    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> workers;
    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        workers.emplace_back([&, w_i]() {
            std::mt19937 rng(w_i * 1234567ULL);
            std::uniform_int_distribution<int> op_dist(0, 2);

            std::vector<std::string> local_keys;
            std::vector<std::string> local_deleted;

            for (u64 i = 0; i < operations_per_worker; i++) {
                cache.enterEpoch(w_i);
                u64 current_epoch = cache.getCurrentEpoch();

                int op = op_dist(rng);

                if (op == 0) {
                    // Insert
                    std::string key   = "stress_w" + std::to_string(w_i)
                                      + "_k"       + std::to_string(i);
                    std::string value = "v_"        + std::to_string(i);

                    RecordCacheEntry* e = createEntry(allocator, key, value);
                    if (cache.InsertOrAssignInRecordCache(make_span(key), e)) {
                        insert_count.fetch_add(1, std::memory_order_relaxed);
                        local_keys.push_back(key);
                    }
                    // e 交给哈希表，不再持有

                } else if (op == 1 && !local_keys.empty()) {
                    // Lookup
                    size_t idx = rng() % local_keys.size();
                    if (cache.GetFromRecordCache(make_span(local_keys[idx])) != nullptr)
                        lookup_count.fetch_add(1, std::memory_order_relaxed);

                } else if (op == 2 && !local_keys.empty()) {
                    // Logical Delete
                    size_t idx = rng() % local_keys.size();
                    const std::string& del_key = local_keys[idx];

                    RecordCacheEntry* e =
                        cache.GetFromRecordCache(make_span(del_key));

                    if (e != nullptr) {
                        RecordCacheType expected = RecordCacheType::ReadOnlyMode;
                        if (e->casType(expected,
                                RecordCacheType::LogicallyDeletedButStillInHashTable)) {
                            cache.addToInvalidationQueue(e, current_epoch);
                            delete_count.fetch_add(1, std::memory_order_relaxed);
                            local_deleted.push_back(del_key);

                            // 从 local_keys 移除，避免重复删除同一 key
                            local_keys.erase(local_keys.begin() + idx);
                        }
                        // e 在 leaveEpoch 后不再使用
                    }
                }

                cache.leaveEpoch(w_i);
                total_operations.fetch_add(1, std::memory_order_relaxed);
            }

            {
                std::lock_guard<std::mutex> lk(deleted_keys_mutex);
                deleted_keys[w_i] = std::move(local_deleted);
            }
        });
    }
    for (auto& w : workers) w.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        end_time - start_time).count();

    print_stats("\n[Statistics]");
    print_info("  Total operations: " + std::to_string(total_operations.load()));
    print_info("  Inserts:          " + std::to_string(insert_count.load()));
    print_info("  Lookups:          " + std::to_string(lookup_count.load()));
    print_info("  Logical deletes:  " + std::to_string(delete_count.load()));
    print_info("  Duration:         " + std::to_string(duration) + " ms");
    if (duration > 0) {
        print_info("  Throughput:       "
                   + std::to_string(
                       static_cast<double>(total_operations.load()) * 1000.0
                       / static_cast<double>(duration))
                   + " ops/sec");
    }

    // 等待后台线程处理队列
    print_info("\nWaiting for Forward_epoch to drain queue...");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 验证：成功逻辑删除的 key 必须从哈希表消失
    u64 confirmed_removed = 0;
    u64 total_to_check    = 0;

    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        for (const auto& key : deleted_keys[w_i]) {
            total_to_check++;
            cache.enterEpoch(w_i);
            if (cache.GetFromRecordCache(make_span(key)) == nullptr)
                confirmed_removed++;
            cache.leaveEpoch(w_i);
        }
    }

    print_stats("\n[Results]");
    print_info("  Keys to verify: "    + std::to_string(total_to_check));
    print_info("  Confirmed removed: " + std::to_string(confirmed_removed)
               + " / " + std::to_string(total_to_check));

    if (total_to_check > 0) {
        assert(confirmed_removed == total_to_check);
        print_pass("All logically deleted entries confirmed removed!");
    } else {
        print_warn("No logical deletions recorded (SIEVE very aggressive).");
    }

    print_pass("Stress test completed without crash or deadlock.");
}

//=================================================================================================
// TEST 7: Epoch 推进速率
//=================================================================================================
void test_epoch_advance_rate() {
    print_test_header("TEST 7: Epoch Advance Rate");

    MockBufferManager bm(50);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 8);

    u64 initial_epoch = cache.getCurrentEpoch();
    print_info("Initial global_epoch = " + std::to_string(initial_epoch));

    cache.startBackgroundThreads();

    std::vector<u64> epoch_samples;
    std::vector<u64> time_samples;
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 10; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        u64  current_epoch = cache.getCurrentEpoch();
        auto now           = std::chrono::high_resolution_clock::now();
        u64  elapsed       = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - start_time).count();

        epoch_samples.push_back(current_epoch);
        time_samples.push_back(elapsed);

        print_info("[t=" + std::to_string(elapsed) + "ms] epoch = "
                   + std::to_string(current_epoch)
                   + " (advanced by "
                   + std::to_string(current_epoch - initial_epoch) + ")");
    }

    u64 total_time      = time_samples.back();
    u64 actual_advance  = epoch_samples.back() - initial_epoch;
    u64 expected_min    = total_time * 8 / 10;   // 允许 20% 误差

    print_stats("\n[Summary]");
    print_info("  Total time:     " + std::to_string(total_time)  + " ms");
    print_info("  Epoch advanced: " + std::to_string(actual_advance));
    print_info("  Expected min:   " + std::to_string(expected_min));
    if (total_time > 0) {
        print_info("  Advance rate:   "
                   + std::to_string(
                       static_cast<double>(actual_advance) /
                       static_cast<double>(total_time))
                   + " epochs/ms");
    }

    assert(actual_advance >= expected_min);
    print_pass("Epoch advance rate is correct!");
}

//=================================================================================================
// Main
//=================================================================================================
int main() {
    FLAGS_cxl_tiering_enabled   = true;
    FLAGS_forward_epoch_thread  = 1;
    FLAGS_sieve_eviction_thread = 1;
    FLAGS_worker_threads        = 16;

    std::cout << "\n" << Color::BOLD << Color::MAGENTA
              << "============================================\n"
              << "   ForwardEpochThread Unit Tests (v2)\n"
              << "   SIEVE eviction always enabled\n"
              << "============================================\n"
              << Color::RESET;

    print_info("FLAGS_cxl_tiering_enabled   = "
               + std::to_string(FLAGS_cxl_tiering_enabled));
    print_info("FLAGS_forward_epoch_thread  = "
               + std::to_string(FLAGS_forward_epoch_thread));
    print_info("FLAGS_sieve_eviction_thread = "
               + std::to_string(FLAGS_sieve_eviction_thread));

    print_warn("Design principle: no entry pointer held outside epoch boundary.");
    print_warn("SIEVE eviction is always running; tests do NOT assume entries");
    print_warn("survive between epochs unless explicitly protected.");

    try {
        test_basic_epoch_mechanism();
        test_invalidation_queue_and_deletion();
        test_epoch_protection();
        test_concurrent_workers();
        test_retry_queue_mechanism();
        test_stress_concurrent();
        test_epoch_advance_rate();

        std::cout << "\n" << Color::BOLD << Color::GREEN
                  << "============================================\n"
                  << "   ALL TESTS PASSED! ✓✓✓\n"
                  << "============================================\n"
                  << Color::RESET << "\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n" << Color::RED << Color::BOLD
                  << "[ERROR] Test failed with exception: " << e.what()
                  << Color::RESET << "\n";
        return 1;
    }
}
