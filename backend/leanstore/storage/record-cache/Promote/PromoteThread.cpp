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
        // [N4-DEFENSE] pmr::memory_resource::allocate never returns nullptr —
        // it either succeeds or throws std::bad_alloc. Our slab allocator throws
        // bad_alloc when (1) the free pool is empty, (2) the carve region is
        // exhausted, AND (3) the synchronous slab rescue callback also failed
        // to free a slab (e.g. all candidate slabs were 100% type=111 / live).
        //
        // Treat that as a "this promote is silently dropped" — the worker that
        // signalled us will simply miss in RecordCache next time and serve the
        // read from the CXL B+Tree path. That is the designed fallback and is
        // strictly safer than propagating an uncaught exception that would kill
        // PromoteThread and leak its registered worker slot.
        void *mem = nullptr;
        try {
            mem = allocator.allocate(total_record_size, alignof(RecordCacheEntry));
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
        if(!mem){
            return nullptr;     // Defensive: should be unreachable, see above.
        }

        auto *entry = new (mem) RecordCacheEntry();
        entry -> tx_ts = 0;
        entry -> last_modified_worker_id = 0;
        entry -> key_length = key_len;
        entry -> value_length = value_len;
        entry -> visited.store(false, std::memory_order_relaxed);
        entry -> swip = Swip<BufferFrame>(request.bf);
        entry -> slot_id = request.slot_id;
        entry -> seqlock.store(0, std::memory_order_relaxed);
        entry -> setType(RecordCacheType::PromoteThreadHoldingThePosition);

        std::memcpy(entry -> payload, request.key.data(), key_len);
        return entry;
    };

    //==============================================================================================
    //      Phase 1: Pre-allocate OUTSIDE shard lock, then decide under lock.
    //
    // [N4-DEFENSE] CRITICAL deadlock fix.
    // Original code called allocate_new_entry() while holding target_shard.mutex.
    // Under memory pressure, allocator.allocate() invokes tryRescueSlabForAllocator(),
    // which iterates ALL shards and takes each shard.mutex as unique_lock — including
    // the one we already hold → self-deadlock that hangs PromoteThread permanently.
    //
    // Fix: pre-allocate before taking the shard lock. If under-lock inspection finds
    // we don't need the entry (key exists in a non-Removed state), release the
    // pre-allocated entry back to the allocator AFTER unlocking the shard. This
    // wastes one alloc/dealloc cycle in the contested case but keeps shard.mutex
    // off the rescue path entirely.
    //==============================================================================================
    RecordCacheEntry* preallocated = allocate_new_entry();
    if(preallocated == nullptr){
        // bad_alloc bubbled up and was swallowed by the lambda — this promote
        // is silently dropped. Worker will fall back to CXL B+Tree on next lookup.
        return;
    }

    bool preallocated_consumed = false;
    {
        std::unique_lock<std::shared_mutex> lock(target_shard.mutex);

        auto existing_item = target_shard.hash_map_shard.find(request.key);
        if(existing_item != target_shard.hash_map_shard.end()){
            RecordCacheEntry * existing_entry = existing_item -> second;
            auto existing_type = existing_entry -> entry_type.load(std::memory_order_acquire);

            if(existing_type != RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation){
                // Don't need the preallocated entry — fall through to release it
                // OUTSIDE the lock (see below). preallocated_consumed stays false.
            }else{
                // Hash table holds a tombstone (still SIEVE FIFO-resident); swap
                // in our fresh entry. The old pointer is still owned by the
                // SIEVE FIFO and will be reclaimed via the InvalidationQueue path.
                existing_item -> second = preallocated;
                new_entry = preallocated;
                preallocated_consumed = true;
            }
        }else{
            // Not Found case:
            target_shard.hash_map_shard.emplace(request.key, preallocated);
            active_entry_count.fetch_add(1, std::memory_order_relaxed);
            new_entry = preallocated;
            preallocated_consumed = true;
        }
    }   // Release shard lock as soon as possible.

    if(!preallocated_consumed){
        // Key already lives in a non-tombstone state — our entry is orphaned.
        // Release it back to the slab allocator. We're outside the shard lock now,
        // so the deallocate (which may invoke tryReclaimSlab) cannot deadlock with
        // any concurrent rescue iteration on this shard.
        const size_t alloc_size = preallocated->totalSizeForRecordCacheEntry();
        preallocated->~RecordCacheEntry();
        allocator.deallocate(preallocated, alloc_size, alignof(RecordCacheEntry));
        return;
    }

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

        // Verify the slot still holds the key the admission scan captured.
        // Concurrent insert/split/merge between admission and now can shift slots,
        // leaving a DIFFERENT key at request.slot_id. Without this check we'd cache
        // (request.key -> wrong key's value) and break lookups.
        //
        // [v4-a fix] request.key carries the SAME bytes that BuildPrefixedKeyOwning
        // produced for the lookup path. When FLAGS_rc_skip_dt_id_prefix=true
        // (YCSB / single-table), there is NO 2-byte dt_id prefix and the request
        // key matches the on-page key 1:1. When false (TPC-C / multi-table), the
        // first 2 bytes are dt_id and must be stripped before comparing. Hardcoding
        // a "-2" here silently made every YCSB promote fail this check and drove
        // RC_HR to 0%.
        {
            const u16 prefix_bytes  = FLAGS_rc_skip_dt_id_prefix ? 0 : 2;
            const u16 page_key_len  = btree_node->getFullKeyLen(request.slot_id);
            if (request.key_length < prefix_bytes
                || page_key_len != static_cast<u16>(request.key_length - prefix_bytes)) {
                jumpmu_return;
            }
            constexpr u16 kMaxKeyBytes = 256;
            if (page_key_len > kMaxKeyBytes) {
                jumpmu_return;
            }
            u8 page_key_buf[kMaxKeyBytes];
            btree_node->copyFullKey(request.slot_id, page_key_buf);
            if (std::memcmp(page_key_buf,
                            reinterpret_cast<const u8*>(request.key.data()) + prefix_bytes,
                            page_key_len) != 0) {
                jumpmu_return;
            }
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
        // Clean up: remove the placeholder from hash table.
        // new_entry is ALREADY in sieve_fifo_queue (pushed at Phase 1.5 above),
        // so we cannot simply allocator.deallocate it here — that would leave a
        // dangling pointer in the FIFO. Instead, hand off to InvalidationQueue
        // so ForwardEpoch frees the slab chunk under epoch safety, and bump the
        // dead_pile counter so SIEVE wakes to drain the FIFO residue.
        {
            std::unique_lock<std::shared_mutex> lock(target_shard.mutex);
            auto it = target_shard.hash_map_shard.find(request.key);
            if (it != target_shard.hash_map_shard.end() && it->second == new_entry) {
                target_shard.hash_map_shard.erase(it);
                active_entry_count.fetch_sub(1, std::memory_order_relaxed);
            }
            new_entry->setType(RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation);
        }
        addToInvalidationQueue(new_entry, getCurrentEpoch());
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
            // Publish as a stable WT entry. seqlock is still 0 (even); subsequent
            // updates will bump it via beginWrite/endWrite.
            new_entry -> setType(RecordCacheType::WriteThroughMode);
            return;
        }
        else if(current_type == RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation){
            // A concurrent remove/eviction unlinked us before we finished promoting.
            // Hash table no longer references new_entry; the SIEVE FIFO we pushed
            // into earlier still owns it and will free via the eviction path.
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