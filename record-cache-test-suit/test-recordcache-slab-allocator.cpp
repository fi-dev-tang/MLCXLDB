#include "backend/leanstore/storage/record-cache/RecordCacheSlabAllocator.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <sys/mman.h>

using namespace leanstore::storage;

// 模拟 BufferManager
class MockBufferManager {
public:
    void* record_cache_ptr;
    size_t total_cache_size;

    MockBufferManager(size_t size_in_mb) {
        total_cache_size = size_in_mb * 1024 * 1024;
        // BufferManager 统一进行一次大块的 mmap 分配
        record_cache_ptr = mmap(nullptr, total_cache_size, PROT_READ | PROT_WRITE, 
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (record_cache_ptr == MAP_FAILED) {
            throw std::bad_alloc();
        }
        std::cout << "BufferManager 预分配了 " << size_in_mb << " MB 的内存作为 Record Cache." << std::endl;
    }

    ~MockBufferManager() {
        if (record_cache_ptr != MAP_FAILED) {
            munmap(record_cache_ptr, total_cache_size);
            std::cout << "BufferManager 释放了预分配的内存." << std::endl;
        }
    }
};

struct CacheRecord {
    uint64_t key;
    char data[248]; // 总大小 256 字节
};

//
// Usage example:
// When we call
// CacheRecord *record1 = alloc.allocate_object<CacheRecord>();
// do_allocate(256)
// --> align_up(256)
// --> get_class_index(256) = 29
// --> classes[29] -> (first usage): lazy loading
//          --------> allocate_slab_for_class(sc)
//          ---------> 1. Cut 2 MB from record_cache_ptr 2. divide into 8192 blocks 3. linked into free_list
//          ---------> Pop a block from free_list
int main() {
    // 1. BufferManager 预分配 100MB 内存
    MockBufferManager buffer_manager(100);

    // 2. 将预分配的指针和大小传给我们的 Slab Allocator
    leanstore::storage::recordcache::RecordCacheSlabAllocator slab_allocator(buffer_manager.record_cache_ptr, buffer_manager.total_cache_size);

    // 3. 使用 pmr::polymorphic_allocator 包装我们的 memory_resource
    std::pmr::polymorphic_allocator<std::byte> alloc(&slab_allocator);

    std::cout << "\n--- 测试 1: 分配自定义结构体 (256B) ---" << std::endl;
    // 这里会触发 256B Size Class 的懒加载，从 BufferManager 的大块内存中划走 2MB，并切分成 256B 的小块
    CacheRecord* record1 = alloc.allocate_object<CacheRecord>();
    record1->key = 1001;
    std::cout << "分配了 CacheRecord, key: " << record1->key << std::endl;
    
    CacheRecord* record2 = alloc.allocate_object<CacheRecord>();
    record2->key = 1002;
    std::cout << "再次分配 CacheRecord, key: " << record2->key << std::endl;

    // 释放内存（会将其放回 256B 的空闲链表头部，不会归还给系统）
    alloc.deallocate_object(record1);
    alloc.deallocate_object(record2);

    std::cout << "\n--- 测试 2: 结合 std::pmr::vector 使用 ---" << std::endl;
    // 创建一个使用我们 allocator 的 pmr::vector
    std::pmr::vector<uint64_t> pmr_vec(&slab_allocator);
    
    // 随着 vector 的扩容，会向 allocator 请求不同大小的内存块
    // 例如 24B, 48B, 96B 等，触发对应 Size Class 的懒加载，每次都会从大块内存中划走 2MB
    for (int i = 0; i < 100; ++i) {
        pmr_vec.push_back(i * 10);
    }
    std::cout << "pmr_vec size: " << pmr_vec.size() << ", capacity: " << pmr_vec.capacity() << std::endl;

    std::cout << "\n--- 测试 3: 结合 std::pmr::string 使用 ---" << std::endl;
    // 创建一个使用我们 allocator 的 pmr::string
    std::pmr::string pmr_str("This is a very long string that bypasses SSO and uses our slab allocator!", &slab_allocator);
    std::cout << "pmr_str: " << pmr_str << std::endl;

    std::cout << "\n测试完成，Allocator 析构时什么也不做，内存由 BufferManager 统一回收。" << std::endl;
    return 0;
}