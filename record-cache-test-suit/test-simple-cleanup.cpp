#include "backend/leanstore/storage/record-cache/RecordCache.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheSlabAllocator.hpp"
#include "backend/leanstore/storage/record-cache/RecordCacheEntry.hpp"
#include "backend/leanstore/Config.hpp"

#include <iostream>
#include <sys/mman.h>
#include <cstring>
#include<thread>

using namespace leanstore::storage::recordcache;

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
        std::cout << "[DEBUG] MockBufferManager allocated " << size_in_mb << "MB at " << record_cache_ptr << std::endl;
    }

    ~MockBufferManager() {
        std::cout << "[DEBUG] MockBufferManager destructor called" << std::endl;
        if (record_cache_ptr != MAP_FAILED) {
            munmap(record_cache_ptr, total_cache_size);
        }
        std::cout << "[DEBUG] MockBufferManager munmap completed" << std::endl;
    }
};

std::span<const uint8_t> make_span(const std::string& str) {
    return {reinterpret_cast<const uint8_t*>(str.data()), str.size()};
}

RecordCacheEntry* createEntry(RecordCacheSlabAllocator& allocator,
                               const std::string& key,
                               const std::string& value)
{
    size_t entry_size = sizeof(RecordCacheEntry) + key.size() + value.size();
    void* mem = allocator.allocate(entry_size, 8);
    RecordCacheEntry* entry = new (mem) RecordCacheEntry();
    
    entry->key_length = key.size();
    entry->value_length = value.size();
    entry->entry_type.store(RecordCacheType::ReadOnlyMode, std::memory_order_release);
    entry->visited.store(false, std::memory_order_release);
    
    std::memcpy(entry->payload, key.data(), key.size());
    std::memcpy(entry->payload + key.size(), value.data(), value.size());
    
    return entry;
}


int main() {
    FLAGS_cxl_tiering_enabled = true;
    FLAGS_forward_epoch_thread = 1;  // ← 启动后台线程
    
    std::cout << "=== Cleanup Test WITH Background Thread ===" << std::endl;
    
    {
        std::cout << "[1] Creating objects..." << std::endl;
        MockBufferManager bm(100);
        RecordCacheSlabAllocator allocator(bm.record_cache_ptr, bm.total_cache_size);
        RecordCache cache(allocator, 8);
        
        std::cout << "[2] Starting background threads..." << std::endl;
        cache.startBackgroundThreads();
        
        std::cout << "[3] Inserting entries..." << std::endl;
        for (int i = 0; i < 10; i++) {
            std::string key = "test_key_" + std::to_string(i);
            std::string value = "value_" + std::to_string(i);
            RecordCacheEntry* entry = createEntry(allocator, key, value);
            cache.InsertOrAssignInRecordCache(make_span(key), entry);
        }
        
        std::cout << "[4] Letting background thread run for 100ms..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        std::cout << "[5] Leaving scope, destructors will run..." << std::endl;
        // cache 析构时必须先 stopBackgroundThreads()
        // 否则后台线程访问已析构的对象 → crash
    }
    
    std::cout << "[6] All destructors completed successfully!" << std::endl;
    std::cout << "    Background thread was properly stopped before cleanup" << std::endl;
    return 0;
}
