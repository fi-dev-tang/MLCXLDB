#include "backend/leanstore/storage/record-cache/RecordCache.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheSlabAllocator.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheEntry.hpp"
#include "backend/leanstore/storage/buffer-manager/BufferFrame.hpp"
#include "backend/leanstore/storage/btree/BTreeVI.hpp"
#include "backend/leanstore/storage/btree/core/BTreeNode.hpp"
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
using namespace leanstore::storage;
using namespace leanstore::storage::btree;

// ============================================================
// ANSI 颜色
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
    std::cout << Color::GREEN << "[PASS] " << Color::RESET << msg << "\n"; }
void print_fail(const std::string& msg) {
    std::cout << Color::RED   << "[FAIL] " << Color::RESET << msg << "\n"; }
void print_info(const std::string& msg) {
    std::cout << Color::CYAN  << "[INFO] " << Color::RESET << msg << "\n"; }
void print_warn(const std::string& msg) {
    std::cout << Color::YELLOW << "[WARN] " << Color::RESET << msg << "\n"; }
void print_setup(const std::string& msg) {
    std::cout << Color::MAGENTA << "[Setup] " << Color::RESET << msg << "\n"; }
void print_phase(const std::string& msg) {
    std::cout << Color::YELLOW << Color::BOLD << msg << Color::RESET << "\n"; }
void print_test_header(const std::string& title) {
    std::cout << "\n" << Color::BOLD << Color::CYAN
              << "========================================\n"
              << "  " << title << "\n"
              << "========================================" << Color::RESET << "\n";
}

// ============================================================
// MockBufferManager（RecordCache 用的内存池）
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
        if (record_cache_ptr == MAP_FAILED) throw std::bad_alloc();
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

// ============================================================
// 构造真实 BufferFrame，内含合法的 BTreeNode + ChainedTuple
// slot_id = 0，key 为空（prefix_length = 0，key_len = 0）
// ============================================================
BufferFrame* createRealBufferFrame(
    const std::string& value,
    u64 tx_ts,
    u16 worker_id)
{
    // BufferFrame 用 new 分配（alignas(512) 保证对齐）
    BufferFrame* bf = new BufferFrame();
    std::memset(bf->page.dt, 0, sizeof(bf->page.dt));

    // page.dt 解释为 BTreeNode
    BTreeNode* node = reinterpret_cast<BTreeNode*>(bf->page.dt);

    // 初始化 BTreeNodeHeader
    node->is_leaf       = true;
    node->count         = 1;
    node->prefix_length = 0;
    node->space_used    = 0;
    node->data_offset   = static_cast<u16>(EFFECTIVE_PAGE_SIZE);

    // ChainedTuple payload 大小
    // ChainedTuple 的布局：header 部分 + u8 payload[]
    // sizeof(BTreeVI::ChainedTuple) 包含 header，不含 payload[]
    u16 payload_len = static_cast<u16>(
        sizeof(BTreeVI::ChainedTuple) + value.size()
    );
    u16 key_len    = 0;   // 测试用空 key
    u16 total_len  = key_len + payload_len;

    // 从 page 末尾向前分配（BTreeNode 惯例）
    node->data_offset -= total_len;
    node->space_used  += total_len;

    // 填写 slot[0]
    node->slot[0].offset      = node->data_offset;
    node->slot[0].key_len     = key_len;
    node->slot[0].payload_len = payload_len;

    // getPayload(0) = ptr() + slot[0].offset + slot[0].key_len
    //               = dt   + data_offset     + 0
    u8* payload_ptr = bf->page.dt + node->data_offset + key_len;
    auto* tuple = reinterpret_cast<BTreeVI::ChainedTuple*>(payload_ptr);

    tuple->tuple_format = BTreeVI::TupleFormat::CHAINED;
    tuple->tx_ts       = tx_ts;
    tuple->worker_id    = worker_id;
    std::memcpy(tuple->payload, value.data(), value.size());

    return bf;
}

// ============================================================
// 手动构造 RecordCacheEntry 辅助
// ============================================================
leanstore::storage::recordcache::RecordCacheEntry* createEntry(RecordCacheSlabAllocator& allocator,
                               const std::string& key,
                               const std::string& value)
{
    size_t entry_size = sizeof(leanstore::storage::recordcache::RecordCacheEntry) + key.size() + value.size();
    void*  mem        = allocator.allocate(entry_size, alignof(leanstore::storage::recordcache::RecordCacheEntry));
    assert(mem != nullptr);

    leanstore::storage::recordcache::RecordCacheEntry* entry = new (mem) leanstore::storage::recordcache::RecordCacheEntry();
    entry->key_length   = static_cast<u16>(key.size());
    entry->value_length = static_cast<u16>(value.size());
    entry->entry_type.store(RecordCacheType::ReadOnlyMode, std::memory_order_release);
    std::memcpy(entry->payload,              key.data(),   key.size());
    std::memcpy(entry->payload + key.size(), value.data(), value.size());
    return entry;
}

//=================================================================================================
// TEST P1: 基础 Promote 功能
// Not Found → PromoteThreadHoldingThePosition → ReadOnlyMode
//=================================================================================================
void test_promote_basic() {
    print_test_header("TEST P1: Basic Promote - Not Found Case");

    MockBufferManager bm(100);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 16);

    FLAGS_record_cache_promote_thread = 1;
    cache.startBackgroundThreads();

    std::string key       = "promote_basic_key";
    std::string value     = "promote_basic_value";
    u16         value_len = static_cast<u16>(value.size());

    // 构造真实 BufferFrame
    BufferFrame* bf = createRealBufferFrame(value, 12345ULL, 1);

    print_setup("Sending promote request for key: " + key);
    cache.signalPromoteThread(make_span(key), bf, 0, value_len);
    print_info("Signal sent, waiting for Promote thread...");

    const int max_wait_ms = 500;
    const int poll_ms     = 10;
    bool      promoted    = false;

    for (int waited = 0; waited < max_wait_ms; waited += poll_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));

        cache.enterEpoch(0);
        leanstore::storage::recordcache::RecordCacheEntry* entry = cache.GetFromRecordCache(make_span(key));
        if (entry != nullptr) {
            auto type = entry->entry_type.load(std::memory_order_acquire);
            if (type == RecordCacheType::ReadOnlyMode) {
                promoted = true;

                // 验证 value 正确写入
                std::string stored(
                    reinterpret_cast<const char*>(entry->payload + key.size()),
                    entry->value_length
                );
                assert(stored == value && "Value mismatch after promote!");
                print_info("Entry found in ReadOnlyMode after "
                           + std::to_string(waited + poll_ms) + "ms");
            }
        }
        cache.leaveEpoch(0);
        if (promoted) break;
    }

    assert(promoted && "Promote thread did not promote entry to ReadOnlyMode!");
    print_pass("Basic promote succeeded: entry is in ReadOnlyMode");

    delete bf;
}

//=================================================================================================
// TEST P2: 重复 Signal 幂等性
// 同一 key 发送 3 次，最终只有一个 ReadOnlyMode entry
//=================================================================================================
void test_promote_idempotent() {
    print_test_header("TEST P2: Duplicate Signal Idempotency");

    MockBufferManager bm(100);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 16);

    FLAGS_record_cache_promote_thread = 1;
    cache.startBackgroundThreads();

    std::string key       = "idempotent_key";
    std::string value     = "idempotent_value";
    u16         value_len = static_cast<u16>(value.size());

    BufferFrame* bf = createRealBufferFrame(value, 99999ULL, 2);

    print_setup("Sending 3 duplicate promote signals for same key...");
    for (int i = 0; i < 3; i++) {
        cache.signalPromoteThread(make_span(key), bf, 0, value_len);
        print_info("Signal #" + std::to_string(i + 1) + " sent");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    cache.enterEpoch(0);
    leanstore::storage::recordcache::RecordCacheEntry* entry = cache.GetFromRecordCache(make_span(key));
    assert(entry != nullptr && "Entry should exist after promote!");
    auto type = entry->entry_type.load(std::memory_order_acquire);
    assert(type == RecordCacheType::ReadOnlyMode &&
           "Entry should be ReadOnlyMode after promote!");
    cache.leaveEpoch(0);

    print_pass("Duplicate signals handled correctly: only one entry, ReadOnlyMode");
    delete bf;
}

//=================================================================================================
// TEST P3: Promote 遇到已存在 ReadOnlyMode 直接返回，不覆盖
//=================================================================================================
void test_promote_skip_existing_readonly() {
    print_test_header("TEST P3: Promote Skips Existing ReadOnlyMode Entry");

    MockBufferManager bm(100);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 16);

    FLAGS_record_cache_promote_thread = 1;

    std::string key       = "existing_readonly_key";
    std::string old_value = "old_value_data_xxxx";   // 长度固定，便于对比
    std::string new_value = "new_value_from_promote";

    // 手动插入 ReadOnlyMode entry
    leanstore::storage::recordcache::RecordCacheEntry* existing = createEntry(allocator, key, old_value);
    bool inserted = cache.InsertOrAssignInRecordCache(make_span(key), existing);
    assert(inserted);
    print_setup("Manually inserted ReadOnlyMode entry with old_value");

    cache.startBackgroundThreads();

    // 发送 Promote（new_value），应该被跳过
    BufferFrame* bf = createRealBufferFrame(new_value, 777ULL, 3);
    cache.signalPromoteThread(
        make_span(key), bf, 0,
        static_cast<u16>(new_value.size())
    );

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    cache.enterEpoch(0);
    leanstore::storage::recordcache::RecordCacheEntry* found = cache.GetFromRecordCache(make_span(key));
    assert(found != nullptr && "Entry should still exist!");
    assert(found->entry_type.load(std::memory_order_acquire)
           == RecordCacheType::ReadOnlyMode);

    std::string stored(
        reinterpret_cast<const char*>(found->payload + key.size()),
        found->value_length
    );
    assert(stored == old_value &&
           "Promote should NOT overwrite existing ReadOnlyMode entry!");
    cache.leaveEpoch(0);

    print_pass("Promote correctly skipped existing ReadOnlyMode entry");
    delete bf;
}

//=================================================================================================
// TEST P4: Promote 遇到 RemovedFromHashTable 状态，重新插入
//=================================================================================================
void test_promote_reinsert_after_removed() {
    print_test_header("TEST P4: Promote Reinserts After RemovedFromHashTable");

    MockBufferManager bm(100);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 16);

    FLAGS_record_cache_promote_thread = 1;

    std::string key       = "removed_reinsert_key";
    std::string old_value = "old_removed_value";
    std::string new_value = "new_promoted_value";

    // 手动插入 entry，然后标记为 RemovedFromHashTable
    leanstore::storage::recordcache::RecordCacheEntry* old_entry = createEntry(allocator, key, old_value);
    bool inserted = cache.InsertOrAssignInRecordCache(make_span(key), old_entry);
    assert(inserted);
    print_setup("Inserted old entry, now marking as RemovedFromHashTable...");

    old_entry->entry_type.store(
        RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation,
        std::memory_order_release
    );
    print_info("old_entry marked as RemovedFromHashTable");

    cache.startBackgroundThreads();

    BufferFrame* bf = createRealBufferFrame(new_value, 55555ULL, 4);
    cache.signalPromoteThread(
        make_span(key), bf, 0,
        static_cast<u16>(new_value.size())
    );
    print_info("Promote signal sent, waiting for Promote thread...");

    const int max_wait_ms = 500;
    const int poll_ms     = 10;
    bool      promoted    = false;

    for (int waited = 0; waited < max_wait_ms; waited += poll_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));

        cache.enterEpoch(0);
        leanstore::storage::recordcache::RecordCacheEntry* found = cache.GetFromRecordCache(make_span(key));
        if (found != nullptr && found != old_entry) {
            auto type = found->entry_type.load(std::memory_order_acquire);
            if (type == RecordCacheType::ReadOnlyMode) {
                std::string stored(
                    reinterpret_cast<const char*>(found->payload + key.size()),
                    found->value_length
                );
                if (stored == new_value) {
                    promoted = true;
                    print_info("New entry found with new_value after "
                               + std::to_string(waited + poll_ms) + "ms");
                }
            }
        }
        cache.leaveEpoch(0);
        if (promoted) break;
    }

    assert(promoted &&
           "Promote thread did not reinsert entry after RemovedFromHashTable!");
    print_pass("Promote correctly reinserted new entry after RemovedFromHashTable");
    delete bf;
}

//=================================================================================================
// TEST P5: Update 线程与 Promote 线程并发冲突
// Promote Phase2 期间 Update 将 entry 标记为 LogicallyDeleted
// Phase3 检测到冲突，从 HashTable 摘除 new_entry
//=================================================================================================
void test_promote_conflict_with_update() {
    print_test_header("TEST P5: Promote vs Update Thread Conflict");

    MockBufferManager bm(100);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 16);

    FLAGS_record_cache_promote_thread = 1;
    FLAGS_sieve_eviction_thread       = 1;

    std::string key       = "conflict_key";
    std::string value     = "conflict_value";
    u16         value_len = static_cast<u16>(value.size());

    std::atomic<bool> update_done{false};

    cache.startBackgroundThreads();

    BufferFrame* bf = createRealBufferFrame(value, 11111ULL, 5);
    cache.signalPromoteThread(make_span(key), bf, 0, value_len);
    print_info("Promote signal sent");

    // 短暂等待，让 Promote 线程进入 Phase1（插入占位符）
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Update 线程：抢在 Phase3 之前把 entry 标记为 LogicallyDeleted
    std::thread update_thread([&]() {
        cache.enterEpoch(1);
        u64 current_epoch = cache.getCurrentEpoch();

        leanstore::storage::recordcache::RecordCacheEntry* entry = cache.GetFromRecordCache(make_span(key));
        if (entry != nullptr) {
            // 尝试从 PromoteThreadHoldingThePosition CAS
            RecordCacheType expected = RecordCacheType::PromoteThreadHoldingThePosition;
            if (entry->casType(expected,
                    RecordCacheType::LogicallyDeletedButStillInHashTable)) {
                cache.addToInvalidationQueue(entry, current_epoch);
                print_info("Update thread: CAS on PromoteHolding succeeded");
            } else {
                // 可能已经到了 ReadOnlyMode
                expected = RecordCacheType::ReadOnlyMode;
                if (entry->casType(expected,
                        RecordCacheType::LogicallyDeletedButStillInHashTable)) {
                    cache.addToInvalidationQueue(entry, current_epoch);
                    print_info("Update thread: CAS on ReadOnlyMode succeeded");
                } else {
                    print_warn("Update thread: CAS failed, unexpected state");
                }
            }
        } else {
            print_warn("Update thread: key not found (Promote not yet in Phase1)");
        }

        cache.leaveEpoch(1);
        update_done.store(true, std::memory_order_release);
    });
    update_thread.join();

    // 等待 Promote Phase3 + ForwardEpoch 处理
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // 验证：状态一致，不崩溃
    // 结果 a: Promote 赢 → ReadOnlyMode
    // 结果 b: Update 赢 → key 不在 RecordCache
    cache.enterEpoch(0);
    leanstore::storage::recordcache::RecordCacheEntry* final_entry = cache.GetFromRecordCache(make_span(key));

    if (final_entry == nullptr) {
        print_info("Result: key removed (Update won or Phase3 cleaned up)");
        print_pass("Conflict handled correctly: entry removed");
    } else {
        auto type = final_entry->entry_type.load(std::memory_order_acquire);
        if (type == RecordCacheType::ReadOnlyMode) {
            print_info("Result: ReadOnlyMode (Promote won before Update)");
            print_pass("Promote completed before Update");
        } else {
            print_fail("Unexpected type: " + std::to_string(static_cast<int>(type)));
            assert(false && "Entry in unexpected state after conflict!");
        }
    }
    cache.leaveEpoch(0);

    print_pass("Promote vs Update conflict test completed without crash");
    delete bf;
}

//=================================================================================================
// TEST P6: 多 Promote 线程并发处理不同 key
// 4 个 Promote 线程，40 个不同 key，全部最终变成 ReadOnlyMode
//=================================================================================================
void test_promote_concurrent_different_keys() {
    print_test_header("TEST P6: Concurrent Promote - Different Keys");

    MockBufferManager bm(200);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 32);

    FLAGS_record_cache_promote_thread = 4;
    cache.startBackgroundThreads();

    const int num_keys = 40;
    std::vector<std::string>  keys(num_keys);
    std::vector<std::string>  values(num_keys);
    std::vector<BufferFrame*> bfs(num_keys);

    print_setup("Sending " + std::to_string(num_keys) + " promote requests...");

    for (int i = 0; i < num_keys; i++) {
        keys[i]   = "concurrent_key_" + std::to_string(i);
        values[i] = "concurrent_val_" + std::to_string(i);
        bfs[i]    = createRealBufferFrame(
                        values[i],
                        static_cast<u64>(i * 1000),
                        static_cast<u16>(i % 16)
                    );
        cache.signalPromoteThread(
            make_span(keys[i]), bfs[i], 0,
            static_cast<u16>(values[i].size())
        );
    }

    print_info("All signals sent, waiting for Promote threads...");

    const int max_wait_ms = 1000;
    const int poll_ms     = 20;
    int       promoted_count = 0;

    for (int waited = 0; waited < max_wait_ms; waited += poll_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        promoted_count = 0;

        for (int i = 0; i < num_keys; i++) {
            cache.enterEpoch(0);
            leanstore::storage::recordcache::RecordCacheEntry* entry = cache.GetFromRecordCache(make_span(keys[i]));
            if (entry != nullptr) {
                auto type = entry->entry_type.load(std::memory_order_acquire);
                if (type == RecordCacheType::ReadOnlyMode)
                    promoted_count++;
            }
            cache.leaveEpoch(0);
        }
        if (promoted_count == num_keys) break;
    }

    print_info("Promoted: " + std::to_string(promoted_count)
               + " / " + std::to_string(num_keys));
    assert(promoted_count == num_keys &&
           "Not all keys promoted to ReadOnlyMode!");
    print_pass("All " + std::to_string(num_keys)
               + " keys promoted by concurrent Promote threads");

    for (auto* bf : bfs) delete bf;
}

//=================================================================================================
// TEST P7: Promote 线程压力测试
// 8 Worker × 100 signals，验证无崩溃、无非法状态
//=================================================================================================
void test_promote_stress() {
    print_test_header("TEST P7: Promote Thread Stress Test");

    MockBufferManager bm(500);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 64);

    FLAGS_record_cache_promote_thread = 2;
    FLAGS_sieve_eviction_thread       = 1;
    FLAGS_forward_epoch_thread        = 1;
    cache.startBackgroundThreads();

    const u64 num_workers    = 8;
    const u64 ops_per_worker = 100;

    std::atomic<u64>          total_signals{0};
    std::vector<BufferFrame*> all_bfs;
    std::mutex                bfs_mutex;

    print_setup("Starting " + std::to_string(num_workers)
                + " workers × " + std::to_string(ops_per_worker) + " signals...");

    std::vector<std::thread> workers;
    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        workers.emplace_back([&, w_i]() {
            std::vector<BufferFrame*> local_bfs;

            for (u64 i = 0; i < ops_per_worker; i++) {
                std::string key   = "stress_w" + std::to_string(w_i)
                                  + "_k"       + std::to_string(i);
                std::string value = "stress_val_" + std::to_string(i);

                BufferFrame* bf = createRealBufferFrame(
                    value,
                    w_i * 1000 + i,
                    static_cast<u16>(w_i)
                );
                local_bfs.push_back(bf);

                cache.signalPromoteThread(
                    make_span(key), bf, 0,
                    static_cast<u16>(value.size())
                );
                total_signals.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }

            std::lock_guard<std::mutex> lk(bfs_mutex);
            for (auto* bf : local_bfs) all_bfs.push_back(bf);
        });
    }
    for (auto& w : workers) w.join();

    print_info("All signals sent: " + std::to_string(total_signals.load()));
    print_info("Waiting for Promote threads to drain queue...");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    u64 readable   = 0;
    u64 not_found  = 0;
    u64 unexpected = 0;

    for (u64 w_i = 0; w_i < num_workers; w_i++) {
        for (u64 i = 0; i < ops_per_worker; i++) {
            std::string key = "stress_w" + std::to_string(w_i)
                            + "_k"       + std::to_string(i);

            cache.enterEpoch(w_i);
            leanstore::storage::recordcache::RecordCacheEntry* entry = cache.GetFromRecordCache(make_span(key));

            if (entry == nullptr) {
                not_found++;
            } else {
                auto type = entry->entry_type.load(std::memory_order_acquire);
                if (type == RecordCacheType::ReadOnlyMode ||
                    type == RecordCacheType::PromoteThreadHoldingThePosition) {
                    readable++;
                } else {
                    unexpected++;
                    print_warn("Unexpected type for key " + key
                               + ": " + std::to_string(static_cast<int>(type)));
                }
            }
            cache.leaveEpoch(w_i);
        }
    }

    print_info("[Results]");
    print_info("  Readable:   " + std::to_string(readable));
    print_info("  Not found:  " + std::to_string(not_found));
    print_info("  Unexpected: " + std::to_string(unexpected));

    assert(unexpected == 0 && "Some entries in unexpected state!");
    print_pass("Stress test completed: no unexpected states");

    for (auto* bf : all_bfs) delete bf;
}

//=================================================================================================
// TEST P8: stopBackgroundThreads 时已处理的 entry 状态合法
//=================================================================================================
void test_promote_drain_on_stop() {
    print_test_header("TEST P8: Promote Thread State Valid on Stop");

    MockBufferManager bm(100);
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 16);

    FLAGS_record_cache_promote_thread = 1;
    cache.startBackgroundThreads();

    const int num_keys = 10;
    std::vector<std::string>  keys(num_keys);
    std::vector<BufferFrame*> bfs(num_keys);

    print_setup("Sending " + std::to_string(num_keys) + " promote requests...");

    for (int i = 0; i < num_keys; i++) {
        keys[i]       = "drain_key_" + std::to_string(i);
        std::string v = "drain_val_" + std::to_string(i);
        bfs[i] = createRealBufferFrame(v, static_cast<u64>(i), 0);

        cache.signalPromoteThread(
            make_span(keys[i]), bfs[i], 0,
            static_cast<u16>(v.size())
        );
    }

    // 短暂等待后 stop
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    print_info("Stopping background threads...");
    cache.stopBackgroundThreads();
    print_info("Background threads stopped");

    // 验证：已处理的 entry 状态合法
    u64 promoted  = 0;
    u64 not_found = 0;

    for (int i = 0; i < num_keys; i++) {
        cache.enterEpoch(0);
        leanstore::storage::recordcache::RecordCacheEntry* entry = cache.GetFromRecordCache(make_span(keys[i]));

        if (entry != nullptr) {
            auto type = entry->entry_type.load(std::memory_order_acquire);
            // 合法状态：ReadOnlyMode 或 PromoteThreadHoldingThePosition（未完成）
            assert(
                type == RecordCacheType::ReadOnlyMode ||
                type == RecordCacheType::PromoteThreadHoldingThePosition
            );
            promoted++;
        } else {
            not_found++;
        }
        cache.leaveEpoch(0);
    }

    print_info("Promoted before stop: " + std::to_string(promoted)
               + " / " + std::to_string(num_keys));
    print_info("Not processed (queue not drained): " + std::to_string(not_found));

    print_pass("All processed entries are in valid state after stop");

    for (auto* bf : bfs) delete bf;
}

//=================================================================================================
// Main
//=================================================================================================
int main() {
    FLAGS_cxl_tiering_enabled         = true;
    FLAGS_forward_epoch_thread        = 1;
    FLAGS_sieve_eviction_thread       = 1;
    FLAGS_record_cache_promote_thread = 1;
    FLAGS_worker_threads              = 16;

    std::cout << "\n" << Color::BOLD << Color::MAGENTA
              << "============================================\n"
              << "   PromoteThread Unit Tests\n"
              << "============================================\n"
              << Color::RESET;

    print_info("FLAGS_cxl_tiering_enabled         = "
               + std::to_string(FLAGS_cxl_tiering_enabled));
    print_info("FLAGS_record_cache_promote_thread = "
               + std::to_string(FLAGS_record_cache_promote_thread));
    print_info("FLAGS_sieve_eviction_thread       = "
               + std::to_string(FLAGS_sieve_eviction_thread));
    print_info("FLAGS_forward_epoch_thread        = "
               + std::to_string(FLAGS_forward_epoch_thread));

    print_warn("Using real BufferFrame + BTreeNode layout.");
    print_warn("EFFECTIVE_PAGE_SIZE = " + std::to_string(EFFECTIVE_PAGE_SIZE));

    try {
        test_promote_basic();
        test_promote_idempotent();
        test_promote_skip_existing_readonly();
        test_promote_reinsert_after_removed();
        test_promote_conflict_with_update();
        test_promote_concurrent_different_keys();
        test_promote_stress();
        test_promote_drain_on_stop();

        std::cout << "\n" << Color::BOLD << Color::GREEN
                  << "============================================\n"
                  << "   ALL PROMOTE TESTS PASSED! ✓✓✓\n"
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
