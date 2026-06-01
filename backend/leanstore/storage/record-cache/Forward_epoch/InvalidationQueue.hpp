#pragma once

#include<mutex>
#include<vector>
#include"Units.hpp"

//------------------------------------[Added].----------------------------------------------------------
// Multi-Producer-Single-Consumer structure for InvalidationQueue
// Forward_epoch:(Consumer), Batch retrieve entries, when epoch is safe, wait for physical reclaimation.
// Worker_thread:(Producer), Put logically deleted RecordCacheEntry to InvalidationQueue
// InvalidationQueue: {RecordCacheEntry *record_cache_entry, Epoch}
namespace leanstore{
namespace storage{
namespace recordcache{

struct RecordCacheEntry;

// WorkerThread who change the RecordCacheEntry'Type to 011 is responsible for adding it to InvalidationQueue.
struct InvalidationEntry{
    RecordCacheEntry* entry_pointer;
    u64 update_epoch;           // The RecordCacheEntry's global_epoch when logically deleted.(Type: 011)
};


//=================================================================================================================
// Multi-Producer-Single-Consumer (MPSC) Queue
//====================================================================================================================
class InvalidationQueue{
public:
    InvalidationQueue(){
        invalidation_queue.reserve(1024);
    }

    // Disable Copy and assignment
    InvalidationQueue(const InvalidationQueue&) = delete;
    InvalidationQueue& operator=(const InvalidationQueue&) = delete;

    // Multi-Producer push:
    // In WT mode the producers are SIEVE eviction, BTree remove, and Promote
    // cleanup; each pushes an entry that is already unlinked from the hash
    // table (Type=100), waiting for ForwardEpoch to perform the epoch-safe
    // slab deallocation.
    void push(RecordCacheEntry* entry, u64 update_epoch){
        std::lock_guard<std::mutex> lock(invalidation_queue_mutex);
        invalidation_queue.push_back({entry, update_epoch});
    }

    // ForwardEpochThread calling:(Single-Consumer)
    // Check all the RecordCacheEntry, prepare for physical reclaimation
    // std::swap O(1)
    // Before:
    // local_queue: []
    // invalidation_queue: [{A,1}, {B,7}, {C,3}, {D,10}]
    //
    // After:
    // local_queue: [{A,1}, {B,7}, {C,3}, {D,10}]
    // invalidation_queue: []
    std::vector<InvalidationEntry> pop_all(){
        std::vector<InvalidationEntry> local_queue;
        {
            std::lock_guard<std::mutex> lock(invalidation_queue_mutex);
            std::swap(local_queue, invalidation_queue);
        }
        return local_queue;
    }

    // Check the approximate size
    size_t approximate_size() const {
        std::lock_guard<std::mutex> lock(invalidation_queue_mutex);
        return invalidation_queue.size();
    }

    // Check if it is empty
    bool empty() const{
        std::lock_guard<std::mutex> lock(invalidation_queue_mutex);
        return invalidation_queue.empty();
    }

private:
    mutable std::mutex invalidation_queue_mutex;
    std::vector<InvalidationEntry> invalidation_queue;
};

}    
}    
}