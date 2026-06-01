#include "RecordCache.hpp"
#include "Units.hpp"
#include "../../Config.hpp"
#include <thread>
#include <chrono>

#include "leanstore/concurrency-recovery/Worker.hpp"
#include "leanstore/concurrency-recovery/CRMG.hpp"

namespace leanstore {
namespace storage {
namespace recordcache {

void RecordCache::startBackgroundThreads()
{
    if (FLAGS_cxl_tiering_enabled) {
        if (FLAGS_forward_epoch_thread) {
            for (u64 t_i = 0; t_i < FLAGS_forward_epoch_thread; t_i++) {
                record_cache_background_threads.emplace_back([this]() {
                    forwardEpochThread();
                });
                bg_threads_counter.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if(FLAGS_sieve_eviction_thread){
            for(u64 t_i = 0; t_i < FLAGS_sieve_eviction_thread; t_i++){
                record_cache_background_threads.emplace_back([this](){
                    sieveEvictionThread();
                });
                bg_threads_counter.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if(FLAGS_record_cache_promote_thread){
            for (u64 t_i = 0; t_i < FLAGS_record_cache_promote_thread; t_i++) {
                record_cache_background_threads.emplace_back([this]() {
                    promoteThread();
                });
                bg_threads_counter.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void RecordCache::stopBackgroundThreads()
{
    bg_threads_keep_running.store(false, std::memory_order_release);

    promote_request_cv.notify_all();        // Wakeup all promoteThread waiting on condition_variable.
    
    // Join all forward_epoch threads to ensure they fully exit before destruction
    for (auto& thread : record_cache_background_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    record_cache_background_threads.clear();
}

//===================================[Added].==========================================================
//                      Lookup Interceptor: tryLookupInRecordCache
//======================================================================================================
bool RecordCache::tryLookupInRecordCache(std::span<const u8> key, 
        const std::function<void(const u8*, u16)>& payload_callback, u64 worker_id)
{
    // 1. Worker thread enterEpoch, {active, current_epoch}
    enterEpoch(worker_id);

    // 2. Check if RecordCache has current record.
    RecordCacheEntry *entry = GetFromRecordCache(key);

    // 3. Check entry's type state
    if(entry != nullptr){
        RecordCacheType current_type = entry -> entry_type.load(std::memory_order_acquire);

        // (3.a): Type::ReadOnlyMode
        if(current_type == RecordCacheType::ReadOnlyMode){
            //=============================================================================
            // [Added]. MVCC Visibility Check
            //=============================================================================
            u16 entry_last_modified_worker_id = entry -> last_modified_worker_id;
            u64 entry_tx_ts = entry -> tx_ts;

            // Calling LeanStore's MVCC Module, check isVisibileForMe
            bool is_visible = cr::Worker::my().cc.isVisibleForMe(entry_last_modified_worker_id, entry_tx_ts, false);

            if(is_visible){
                // Pass the visibility check, can safely read it
                const u8* payload_ptr = reinterpret_cast<const u8*>(entry) + sizeof(RecordCacheEntry) + entry -> key_length;

                // payload return result.
                payload_callback(payload_ptr, entry -> value_length);

                // SIEVE visited logic
                SieveFIFOQueue::OnSieveFIFOAccess(entry);
                leaveEpoch(worker_id);
                return true;
            } else {
                // If not visible, fallback to BTree to traverse the version chain
                leaveEpoch(worker_id);
                return false;
            }
        }
        else{
            leaveEpoch(worker_id);
            return false;
        }
    }

    leaveEpoch(worker_id);
    return false;
}
//===================================[Added].==========================================================
//                      Update Interceptor: tryUpdateAndInvalidateRecordCache
//======================================================================================================
bool RecordCache::tryUpdateAndInvalidateRecordCache(std::span<const u8> key, u64 worker_id){
    // 1. Worker thread enterEpoch, {active, current_epoch}
    enterEpoch(worker_id);
    
    // 2. Check if RecordCache has current record.
    RecordCacheEntry *entry = GetFromRecordCache(key);

    // 3. Check entry's state
    if(entry == nullptr){
        leaveEpoch(worker_id);
        return false;
    }

    // 4. Check RecordCacheEntry's Type state:
    RecordCacheType current_type = entry -> entry_type.load(std::memory_order_acquire);

    // (4.a). Type::ReadOnlyMode, can safely set invalidation bit.
    // (4.b). Type::PromoteThreadHoldingThePostion
    // Promote old value, invalidate here.
    if(current_type == RecordCacheType::ReadOnlyMode || 
    current_type == RecordCacheType::PromoteThreadHoldingThePosition){
        // CAS to RecordCacheType::LogicallyDeletedButStillInHashTable
        if(entry -> casType(current_type, RecordCacheType::LogicallyDeletedButStillInHashTable)){
            addToInvalidationQueue(entry, getCurrentEpoch());
            leaveEpoch(worker_id);
            return true;
        }
        leaveEpoch(worker_id);
        return false;
    }else{
        leaveEpoch(worker_id);
        return false;
    }
}

}  // namespace recordcache
}  // namespace storage
}  // namespace leanstore