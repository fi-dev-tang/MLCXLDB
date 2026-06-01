#include "../RecordCache.hpp"
#include<span>
#include<pthread.h>
#include"../RecordCacheEntry.hpp"
#include"../RecordCacheSlabAllocator.hpp"
#include"Units.hpp"
#include"../../btree/BTreeVI.hpp"
#include"../../btree/core/BTreeNode.hpp"
#include"../../buffer-manager/BMPlainGuard.hpp"

namespace leanstore{
namespace storage{
namespace recordcache{

// Condition_variable wait signal.
void RecordCache::promoteThread(){
    std::string thread_name("rc_promote");
    pthread_setname_np(pthread_self(), thread_name.c_str());

    while(bg_threads_keep_running.load(std::memory_order_acquire)){
        PromoteRequestMessage request;

        // Accept Signal from WorkerThread(lookup procedure)
        {
            std::unique_lock<std::mutex> lock(promote_request_queue_mutex);

            promote_request_cv.wait(lock,[this]{
                return !promote_request_message_queue.empty() || !bg_threads_keep_running.load(std::memory_order_acquire);
            });

            if(!bg_threads_keep_running.load(std::memory_order_acquire) && promote_request_message_queue.empty()){
                break;
            }
            request = std::move(promote_request_message_queue.front());
            promote_request_message_queue.pop();
        }
        processOnPromotionRequest(request);
    }
    bg_threads_counter.fetch_sub(1, std::memory_order_release);
}

//-----------------------------------[Added].----------------------------------------
// Core function:
// Promote thread has the only function as Promote, it does not handle physical memory deallocation(EvictionThread's responsibility).
// Here we avoid all the conflict situation, only does the three thing
// 1. Check if the inserted key is not in RecordCache(Only NotFond or RemovedFromHashTableButWaitForPhysicalMemoryDeallocation)
// 2. If not found, then we read CXL BufferFrame to our thread_local_cache(without holding lock)
// 3. Hold lock, double-check if conflict
//    Not conflict, then finish the Promotion Procedure.
// [Only Two cases allowed] Type Machine: Not Found or RemovedFromHashTableButWaitForPhysicalMemoryDeallocation
void RecordCache::processOnPromotionRequest(const PromoteRequestMessage& request){
    // [FIX-B] Producer-side hard backpressure against OOM.
    // The slab allocator is bounded; once full, do_allocate throws
    // std::bad_alloc which terminates the process. We check the slab usage
    // ratio BEFORE allocating and bail out early if we're above the hard
    // watermark. The dropped decision is not a permanent loss: admission
    // (CheckAndPromote) will re-emit it on a future round if the record
    // remains hot.
    static constexpr double kPromoteHardWatermark = 0.95;
    if (allocator.getUsageRatio() > kPromoteHardWatermark) {
        promote_rejected_high_water.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::span<const u8> key_span(reinterpret_cast<const u8*>(request.key.data()),
    request.key.size());

    const u64 hash_value = HashBytes(key_span);
    auto& target_shard = getShardByHash(hash_value);        // auto& target_shard = this -> getShard(key_span);

    RecordCacheEntry *new_entry = nullptr;
    const u16 key_len = static_cast<u16>(request.key.size());
    const u16 value_len = static_cast<u16>(request.value_length);
    size_t total_record_size = sizeof(RecordCacheEntry) + key_len + value_len;

    // lambda expression for allocation new_entry
    auto allocate_new_entry = [&]() -> RecordCacheEntry*{
        // Allocate memory from RecordCacheSlabAllocator
        void *mem = allocator.allocate(total_record_size, alignof(RecordCacheEntry));
        if(!mem){
            return nullptr;     // Memory Allocation failed
        }

        auto *entry = new (mem) RecordCacheEntry();
        entry -> tx_ts = 0;
        entry -> last_modified_worker_id = 0;
        entry -> key_length = key_len;
        entry -> value_length = value_len;
        entry -> visited.store(false, std::memory_order_relaxed);
        entry -> setType(RecordCacheType::PromoteThreadHoldingThePosition);

        std::memcpy(entry -> payload, request.key.data(), key_len);

        // [FIX-A] active_entry_count tracks SLAB occupancy, not hash-map count.
        // Pair this +1 with the -1 in SIEVE Case A / Case B (the only places
        // that physically deallocate). forward_epoch removing from hashmap
        // does NOT decrement (slab still in use until SIEVE pops).
        active_entry_count.fetch_add(1, std::memory_order_relaxed);
        return entry;
    };

    //==============================================================================================
    //      Phase 1: Get Hash Shard Lock and check RecordCacheType
    // RemovedFromHashTable -> existing_item -> second = new_entry(overwrite)
    // Not Found -> hash_map_shard.emplace() (insert)
    //==============================================================================================
    {
        std::unique_lock<std::shared_mutex> lock(target_shard.mutex);

        auto existing_item = target_shard.hash_map_shard.find(request.key);
        if(existing_item != target_shard.hash_map_shard.end()){
            RecordCacheEntry * existing_entry = existing_item -> second;
            auto existing_type = existing_entry -> entry_type.load(std::memory_order_acquire);

            if(existing_type != RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation){
                return;
            }
            // Here is the core execution
            // Key not found or RemovedFromHashTableButWaitForPhysicalMemoryDeallocation
            // [Note]:
            // In our design, RecordCacheEntry * will have 2 owner (RecordCache & SIEVE_FIFO_QUEUE)
            // Here if entry_type marked RemovedFromHashTable..., it still exist in SIEVE_FIFO_QUEUE
            // we can not just directly reuse its memory, need to allocate for new_entry.

             //======================================================================================================================================
            // Core Execution:
            // Only Not Found or
            // Phase 1: Hold lock,
            // insert RecordCacheType::PromoteThreadHoldingThePosition Placeholder
            //======================================================================================================================================
            new_entry = allocate_new_entry();
            if(!new_entry) return;
            // The value part is not inserted for CXL read latency
            existing_item -> second = new_entry;

        }else{
            // Not Found case:
            //======================================================================================================================================
            // Core Execution:
            // Only Not Found or
            // Phase 1: Hold lock,
            // insert RecordCacheType::PromoteThreadHoldingThePosition Placeholder
            //======================================================================================================================================
            new_entry = allocate_new_entry();
            if(!new_entry) return;
            target_shard.hash_map_shard.emplace(request.key, new_entry);
            // [FIX-A] active_entry_count was already incremented inside
            // allocate_new_entry() when the slab block was actually obtained.
            // Do NOT increment again here — that would double-count slab usage.
        }

    }   // Release lock as soon as possible.

    // FIFO enqueue outside shard lock(avoid deadlock with eviction path).
    if(new_entry != nullptr) sieve_fifo_queue.InsertIntoSieveFIFO(new_entry);

    //==========================================================================================================================================
    // Core Execution:
    // Phase 2: Not Holding lock,
    // Reading context from CXL BufferFrame to thread_local_cache.
    // Protected by OptimisticGuard: if the page is concurrently modified (split/compact/update),
    // the recheck() will longjmp and we abort this promote attempt.
    //==========================================================================================================================================
    std::vector<u8> local_value_buffer;
    local_value_buffer.resize(value_len);

    u64 local_tx_ts = 0;
    u16 local_worker_id = 0;
    bool cxl_read_success = false;

    jumpmuTry() {
        BMOptimisticGuard cxl_opt_guard(request.bf->header.latch);

        if (request.bf->header.pid != request.pid ||
            request.bf->header.state != leanstore::storage::BufferFrame::STATE::HOT) {
            jumpmu_return;
        }

        auto* btree_node = reinterpret_cast<leanstore::storage::btree::BTreeNode*>(request.bf->page.dt);
        if (request.slot_id >= btree_node->count) {
            jumpmu_return;
        }

        u8* payload = btree_node->getPayload(request.slot_id);
        auto* tuple = reinterpret_cast<leanstore::storage::btree::BTreeVI::Tuple*>(payload);

        using TupleFormat = leanstore::storage::btree::BTreeVI::TupleFormat;

        local_tx_ts = tuple->tx_ts;
        local_worker_id = tuple->worker_id;

        if (tuple->tuple_format == TupleFormat::CHAINED) {
            auto* chained_tuple = reinterpret_cast<leanstore::storage::btree::BTreeVI::ChainedTuple*>(payload);
            std::memcpy(local_value_buffer.data(), chained_tuple->payload, value_len);
        } else if (tuple->tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
            auto* fat_tuple = reinterpret_cast<leanstore::storage::btree::BTreeVI::FatTupleDifferentAttributes*>(payload);
            std::memcpy(local_value_buffer.data(), fat_tuple->getValue(), value_len);
        } else {
            std::memcpy(local_value_buffer.data(), payload + sizeof(leanstore::storage::btree::BTreeVI::Tuple), value_len);
        }

        cxl_opt_guard.recheck();
        cxl_read_success = true;
    } jumpmuCatch() {
        // Optimistic read failed (page was concurrently modified) — abort this promote.
    }

    if (!cxl_read_success) {
        // Clean up: remove the placeholder from hash table
        std::unique_lock<std::shared_mutex> lock(target_shard.mutex);
        auto it = target_shard.hash_map_shard.find(request.key);
        if (it != target_shard.hash_map_shard.end() && it->second == new_entry) {
            target_shard.hash_map_shard.erase(it);
            // [FIX-A] do NOT decrement active_entry_count here — slab is still
            // allocated; entry will be reclaimed by SIEVE Case A which owns
            // the matching -1.
        }
        new_entry->setType(RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation);
        // [FIX-C] entry dropped directly to state=100; signal SIEVE
        IncrementPendingStateInvalidatedFromHash();
        return;
    }

    //==========================================================================================================================================
    // Core Execution: 
    // Phase 3: Holding lock, 
    // double-check, write value back to new_entry
    // PromoteThreadHoldingThePosition -> ReadOnly: success
    // LogicallyDeletedButStillInHashTable -> RemovedFromHashTableButWaitForPhysicalMemoryDeallocation: Eviction Thread handle it.
    //==========================================================================================================================================
    {
        std::unique_lock<std::shared_mutex> lock(target_shard.mutex);

        auto current_type = new_entry -> entry_type.load(std::memory_order_acquire);
        if(current_type == RecordCacheType::PromoteThreadHoldingThePosition){
            // double-check succeed.
            // Copy from thread_local_cache.
            std::memcpy(new_entry -> payload + key_len, local_value_buffer.data(), value_len);
            new_entry -> tx_ts = local_tx_ts;
            new_entry -> last_modified_worker_id = local_worker_id;
            new_entry -> setType(RecordCacheType::ReadOnlyMode);
            return;
        }
        
        else if(current_type == RecordCacheType::LogicallyDeletedButStillInHashTable){
            // Other Worker(Update Procedure has changed type to LogicallyDeletedButStillInHashTable)
            // Promote failed.
            auto it = target_shard.hash_map_shard.find(request.key);
            if(it != target_shard.hash_map_shard.end() && it -> second == new_entry){
                target_shard.hash_map_shard.erase(it);
                // [FIX-A] do NOT decrement active_entry_count here — slab still
                // in use; SIEVE Case A will release it (and decrement).
            }
            new_entry -> setType(RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation);
            // [FIX-C] entry dropped directly to state=100; signal SIEVE
            IncrementPendingStateInvalidatedFromHash();
            return;
        }
        else{
            assert(false && "Error, Promote Thread will not create other logic");
        }
    }
}
}
}
}