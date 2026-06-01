#include "backend/leanstore/storage/record-cache/RecordCache.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheSlabAllocator.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheEntry.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <cassert>
#include <sys/mman.h>
#include <atomic>
#include <random>

using namespace leanstore::storage::recordcache;

// 模拟 BufferManager 预分配大块内存
class MockBufferManager {
public:
    void* record_cache_ptr;
    size_t total_cache_size;

    MockBufferManager(size_t size_in_mb) {
        total_cache_size = size_in_mb * 1024 * 1024;
        record_cache_ptr = mmap(nullptr, total_cache_size, PROT_READ | PROT_WRITE, 
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
};

// 辅助函数：将 string 转换为 span
std::span<const uint8_t> make_span(const std::string& str) {
    return {reinterpret_cast<const uint8_t*>(str.data()), str.size()};
}

void test_single_thread() {
    std::cout << "[Test] Single Thread Operations..." << std::endl;
    MockBufferManager bm(100); // 100MB
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    RecordCache cache(allocator, 16); // 16 shards

    std::string key1 = "key_001";
    std::string key2 = "key_002";

    // 模拟分配 RecordCacheEntry
    // 注意：实际项目中会通过 allocator 分配，这里为了测试接口正确性，我们简单 new 出来
    // 但因为 RecordCacheEntry 是变长结构体，我们需要分配足够的空间
    size_t entry_size = sizeof(RecordCacheEntry) + 128; 
    RecordCacheEntry* entry1 = reinterpret_cast<RecordCacheEntry*>(malloc(entry_size));
    entry1->key_length = key1.size();
    entry1->value_length = 10;
    entry1->setType(RecordCacheType::ReadOnlyMode);

    RecordCacheEntry* entry2 = reinterpret_cast<RecordCacheEntry*>(malloc(entry_size));
    entry2->key_length = key2.size();
    entry2->value_length = 20;
    entry2->setType(RecordCacheType::WriteThroughMode);

    // 测试 Insert
    bool inserted1 = cache.InsertOrAssignInRecordCache(make_span(key1), entry1);
    assert(inserted1 == true);
    
    bool inserted2 = cache.InsertOrAssignInRecordCache(make_span(key2), entry2);
    assert(inserted2 == true);

    // 测试 Get
    RecordCacheEntry* res1 = cache.GetFromRecordCache(make_span(key1));
    assert(res1 == entry1);
    assert(res1->isExpectedType(RecordCacheType::ReadOnlyMode));

    RecordCacheEntry* res2 = cache.GetFromRecordCache(make_span(key2));
    assert(res2 == entry2);

    RecordCacheEntry* res3 = cache.GetFromRecordCache(make_span("not_exist"));
    assert(res3 == nullptr);

    // 测试 Assign (Update existing)
    bool inserted3 = cache.InsertOrAssignInRecordCache(make_span(key1), entry2);
    assert(inserted3 == false); // 应该返回 false，表示更新了已存在的 key
    RecordCacheEntry* res_updated = cache.GetFromRecordCache(make_span(key1));
    assert(res_updated == entry2);

    // 测试 Erase
    bool erased = cache.EraseFromRecordCache(make_span(key1));
    assert(erased == true);
    assert(cache.GetFromRecordCache(make_span(key1)) == nullptr);

    erased = cache.EraseFromRecordCache(make_span("not_exist"));
    assert(erased == false);

    free(entry1);
    free(entry2);
    std::cout << "[Test] Single Thread Operations Passed!" << std::endl;
}

void test_multi_thread_concurrency() {
    std::cout << "[Test] Multi-Thread Concurrency (64 Threads)..." << std::endl;
    MockBufferManager bm(500); // 500MB
    RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
    
    const int num_threads = 64;
    const int keys_per_thread = 1000;
    RecordCache cache(allocator, 256); // 256 shards to reduce contention

    std::atomic<int> total_inserted{0};
    std::atomic<int> total_found{0};

    auto worker = [&](int thread_id) {
        std::vector<RecordCacheEntry*> local_entries;
        
        // 1. Insert Phase
        for (int i = 0; i < keys_per_thread; ++i) {
            std::string key = "key_" + std::to_string(thread_id) + "_" + std::to_string(i);
            
            size_t entry_size = sizeof(RecordCacheEntry) + key.size() + 8;
            RecordCacheEntry* entry = reinterpret_cast<RecordCacheEntry*>(malloc(entry_size));
            entry->key_length = key.size();
            entry->value_length = 8;
            entry->setType(RecordCacheType::ReadOnlyMode);
            local_entries.push_back(entry);

            bool inserted = cache.InsertOrAssignInRecordCache(make_span(key), entry);
            if (inserted) {
                total_inserted.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // 2. Read Phase (Lookup)
        for (int i = 0; i < keys_per_thread; ++i) {
            std::string key = "key_" + std::to_string(thread_id) + "_" + std::to_string(i);
            RecordCacheEntry* res = cache.GetFromRecordCache(make_span(key));
            if (res != nullptr) {
                total_found.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // 3. Mixed Read/Write Phase (Contention test)
        std::mt19937 rng(thread_id);
        std::uniform_int_distribution<int> dist(0, num_threads - 1);
        
        for (int i = 0; i < 500; ++i) {
            // Randomly read other threads' keys
            int target_thread = dist(rng);
            std::string key = "key_" + std::to_string(target_thread) + "_" + std::to_string(i % keys_per_thread);
            cache.GetFromRecordCache(make_span(key));
            
            // Randomly erase own keys
            if (i % 10 == 0) {
                std::string erase_key = "key_" + std::to_string(thread_id) + "_" + std::to_string(i);
                cache.EraseFromRecordCache(make_span(erase_key));
            }
        }

        // Clean up memory
        for (auto* entry : local_entries) {
            free(entry);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "  Total Inserted: " << total_inserted.load() << " (Expected: " << num_threads * keys_per_thread << ")" << std::endl;
    std::cout << "  Total Found immediately after insert: " << total_found.load() << " (Expected: " << num_threads * keys_per_thread << ")" << std::endl;
    
    assert(total_inserted.load() == num_threads * keys_per_thread);
    assert(total_found.load() == num_threads * keys_per_thread);

    std::cout << "[Test] Multi-Thread Concurrency Passed!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "   Running RecordCache Unit Tests       " << std::endl;
    std::cout << "========================================" << std::endl;

    test_single_thread();
    std::cout << "----------------------------------------" << std::endl;
    test_multi_thread_concurrency();

    std::cout << "========================================" << std::endl;
    std::cout << "   All Tests Passed Successfully!       " << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
