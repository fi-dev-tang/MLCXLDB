#pragma once

#include<memory_resource>
#include<vector>
#include<mutex>
#include<cassert>
#include<stdexcept>
#include"Units.hpp"
//----------------------------------------------[Added].------------------------------------------------------
// SlabAllocator for RecordCacheEntry
// 1. Take ownership of BufferManager's memory chunk(use mmap to allocate before.)
// 2. 2MB slice and 8 Byte step, start from 24 B ... to 16 KB
// 3. Lazy loading, if some SlabClass, for example 256 B is never used, then do not allocate it.
// 4. Integrate it with c++17 pmr interface.

namespace leanstore{
namespace storage{
namespace recordcache{

// Using C++17 std::pmr
// We override do_allocate and do_deallocate for RecordCacheSlabAllocator
class RecordCacheSlabAllocator: public std::pmr::memory_resource{
public:
    static constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;   // 2 MB, allocating huge page at a time.
    static constexpr size_t BLOCK_STEP = 8;                      // each size class has 8 Byte difference, for example: 32 B, 40B, 48B, 56B ...
    static constexpr size_t MIN_BLOCK_SIZE = 24;                // Minimal size: 16 B (RecordCacheEntry) + 8 Byte Payload
    static constexpr size_t MAX_BLOCK_SIZE = 16 * 1024;         // Maximal size: 16 KB(Up to Page Size)
    static constexpr size_t NUM_CLASSES = (MAX_BLOCK_SIZE - MIN_BLOCK_SIZE) / BLOCK_STEP + 1;

    // OS(mmap)
    // --> record_cache_ptr (a huge size of pre-allocated memory)
    // SlabClass[0] ( 24 Bytes)
    // -> SlabClass[1] ( 32 Bytes)
    // -> ...  
    // -> SlabClass [N] (16 KB)
    // Takes ownership of a pre-allocated memory region starting at record_cache_ptr(allocated via mmap by BufferManager).
    // SlabAllocator manages this region internally:
    // 1. Divides it into 2 MB slabs
    // 2. Each slab is assigned to a SlabClass (24, 32, ... bytes)
    // 3. Maintains per-class free_list to eliminate external fragmentation
    // 4. 8-byte step minimizes internal fragmentation

    RecordCacheSlabAllocator(void *base_ptr, size_t total_size)
    : base_ptr(static_cast<char*>(base_ptr)),
    current_ptr(static_cast<char*>(base_ptr)),
    end_ptr(static_cast<char*>(base_ptr) + total_size),
    total_capacity(total_size)
    {
        if(!base_ptr || total_size < HUGE_PAGE_SIZE){
            throw std::invalid_argument("Invalid base pointer: check BufferManager:: record_cache_ptr mmap");
        }

        for(size_t i = 0; i < NUM_CLASSES; i++){
            classes[i].block_size = MIN_BLOCK_SIZE + i * BLOCK_STEP;
            classes[i].free_list = nullptr;             // Lazy loading, only when we really need block_size's chunk, then it is not nullptr
        }
    }

    ~RecordCacheSlabAllocator() override = default;

    // Get current usage ratio
    double getUsageRatio() const {
        if(total_capacity == 0) return 0.0;
        return static_cast<double>(bytes_in_use.load(std::memory_order_relaxed)) /
        static_cast<double>(total_capacity);
    }

    size_t getBytesInUse() const {
        return bytes_in_use.load(std::memory_order_relaxed);
    }

    size_t getTotalCapacity() const {
        return total_capacity;
    }

    size_t getAlignedBlockSize(size_t raw_bytes) const {
        return align_up(raw_bytes);
    }

    // Disable Copy and Move
    RecordCacheSlabAllocator(const RecordCacheSlabAllocator&) = delete;
    RecordCacheSlabAllocator& operator=(const RecordCacheSlabAllocator&) = delete;

protected:
    void* do_allocate(size_t bytes, size_t alignment) override{
        // Check that we have 8B alignment
        assert(alignment <= 8 && "RecordCacheSlabAllocator only supports up to 8-byte alignment");

        // if larger then the largest block size, withdraw back to new_delete_resource
        if(bytes > MAX_BLOCK_SIZE){
            return std::pmr::new_delete_resource() -> allocate(bytes, alignment);
        }

        size_t slab_class_index = GetClassIndex(bytes);
        auto &slab_size_class = classes[slab_class_index];

        std::lock_guard<std::mutex> lock(slab_size_class.mutex);

        // Lazy loading, if the size class has no free chunk, then cut 2 MB Huge Page from record_cache_ptr
        if(!slab_size_class.free_list){
            allocate_slab_for_class(slab_size_class);
        }

        FreeNode *node = slab_size_class.free_list;
        slab_size_class.free_list = node -> next;

        bytes_in_use.fetch_add(slab_size_class.block_size, std::memory_order_relaxed);
        return static_cast<void*>(node);
    }

    void do_deallocate(void *p, size_t bytes, size_t alignment) override{
        if(!p) return;

        if(bytes > MAX_BLOCK_SIZE){
            std::pmr::new_delete_resource() -> deallocate(p, bytes, alignment);
            return;
        }

        size_t slab_class_index = GetClassIndex(bytes);
        auto &slab_size_class = classes[slab_class_index];

        std::lock_guard<std::mutex> lock(slab_size_class.mutex);

        // Return the freed block, insert to the header of linked list
        FreeNode * node = static_cast<FreeNode*>(p);
        node -> next = slab_size_class.free_list;
        slab_size_class.free_list = node;

        bytes_in_use.fetch_sub(slab_size_class.block_size, std::memory_order_relaxed);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    struct FreeNode{
        FreeNode *next;
    };

    struct SlabSizeClass{
        std::mutex mutex;           // Every SlabSizeClass's concurrency safety.
        FreeNode* free_list;
        size_t block_size;
    };

    SlabSizeClass classes[NUM_CLASSES];

    // Global Memory Management
    // Start from base_ptr, every time we have *current_ptr + 2 MB
    char *base_ptr;                                         
    char *current_ptr;
    char *end_ptr;
    size_t total_capacity;
    std::mutex already_allocated_large_chunk_memory;        // Protect that we cut 2 MB slab from record_cache_ptr
    std::atomic<size_t> bytes_in_use{0};

    size_t align_up(size_t size) const{
        if(size <= MIN_BLOCK_SIZE) return MIN_BLOCK_SIZE;
        return (size + BLOCK_STEP - 1) & ~(BLOCK_STEP - 1);
    }

    size_t GetClassIndex(size_t size) const {
        size_t aligned = align_up(size);
        assert(aligned <= MAX_BLOCK_SIZE);

        return (aligned - MIN_BLOCK_SIZE) / BLOCK_STEP;
    }

    // If we require a slab_size_class.slab_size 's chunk
    // Then we (1) require 2 MB from record_cache_ptr (2) cut the 2 MB to form a slab_sized free linked-list.
    void allocate_slab_for_class(SlabSizeClass &slab_size_class){
        char *slab = nullptr;

        // Step 1: Cut 2 MB from already allocated large chunk of memory
        {   
            std::lock_guard<std::mutex> lock(already_allocated_large_chunk_memory);
            if(current_ptr + HUGE_PAGE_SIZE > end_ptr){
                throw std::bad_alloc();         // The pre-allocated memory has been used up!
            }
            slab = current_ptr;
            current_ptr += HUGE_PAGE_SIZE;
        }

        size_t block_size = slab_size_class.block_size;
        size_t num_of_that_blocks = HUGE_PAGE_SIZE / block_size;

        // Step 2: Cut 2 MB's Slab into block_size's chunk, and chain it to a linkedlist
        for(size_t i = 0; i < num_of_that_blocks - 1; i++){
            FreeNode* node = reinterpret_cast<FreeNode*>(slab + i * block_size);
            node -> next = reinterpret_cast<FreeNode*>(slab + (i + 1) * block_size);
        }
        FreeNode* last_node = reinterpret_cast<FreeNode*>(slab + (num_of_that_blocks - 1) * block_size);
        last_node -> next = nullptr;

        slab_size_class.free_list = reinterpret_cast<FreeNode*>(slab);
    }
};

}
}
}

//
// Usage example:
// (1) Wrapper part:
// Using pmr::polymorphic_allocator to wrap our memory_resource
//
// leanstore::storage::recordcache::RecordCacheSlabAllocator slab_allocator(buffer_manager.record_cache_ptr, FLAGS_dram_record_cache_gib);
// std::pmr::polymorphic_allocator<std::byte> alloc(&slab_allocator);
// 
// (2) Usage part: If a RecordCacheEntry has 256 B.
// RecordCacheEntry * record_1 = alloc.allocate_object<RecordCacheEntry>();
// 
// [Inner function]:
// do_allocate(256)
// --> align_up(256)
// --> get_class_index(256) = 29
// --> classes[29] -> (first usage): lazy loading
//          --------> allocate_slab_for_class(sc)
//          ---------> 1. Cut 2 MB from record_cache_ptr 2. divide into 8192 blocks 3. linked into free_list
//          ---------> Pop a block from free_list