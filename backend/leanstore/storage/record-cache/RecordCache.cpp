#include "RecordCache.hpp"
#include "Units.hpp"
#include "../../Config.hpp"
#include <thread>
#include <chrono>
#include <cstring>

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

    for (auto& thread : record_cache_background_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    record_cache_background_threads.clear();
}

//===================================[Added].==========================================================
//                      Lookup Interceptor: tryLookupInRecordCache
//
// SeqLock-based optimistic read:
//   1. enterEpoch (protect entry pointer from being freed mid-read)
//   2. find entry under shard's shared lock
//   3. SeqLock dance:
//        loop:
//          start = entry->beginRead()  // spins while writer holds odd version
//          type  = entry->entry_type
//          if type != WriteThroughMode → fall through to CXL
//          snapshot tx_ts / worker_id / value into local buffers
//          if entry->retryRead(start) → snapshot is consistent, break
//   4. MVCC visibility check on snapshot
//   5. payload_callback(snapshot value)
//   6. SIEVE visited bit
//   7. leaveEpoch
//======================================================================================================
bool RecordCache::tryLookupInRecordCache(std::span<const u8> key,
        const std::function<void(const u8*, u16)>& payload_callback, u64 worker_id)
{
    enterEpoch(worker_id);

    RecordCacheEntry* entry = GetFromRecordCache(key);
    if (entry == nullptr) {
        leaveEpoch(worker_id);
        return false;
    }

    // Local snapshot buffers — sized lazily once we know value_length.
    // value_length is read inside the seqlock region so the buffer can move.
    thread_local std::vector<u8> local_value_buffer;

    u16 snap_value_length = 0;
    u16 snap_worker_id = 0;
    u64 snap_tx_ts = 0;
    RecordCacheType snap_type = RecordCacheType::WriteThroughMode;

    // Bound the SeqLock retry loop so we never spin forever on a livelock.
    constexpr int kMaxSeqRetry = 8;
    bool snapshot_consistent = false;
    for (int attempt = 0; attempt < kMaxSeqRetry; ++attempt) {
        u64 start_v = entry->beginRead();

        snap_type = entry->entry_type.load(std::memory_order_acquire);
        if (snap_type != RecordCacheType::WriteThroughMode) {
            // Not a stable hit: 100 (unlinked) / 111 (promote in progress).
            // Drop snapshot work and fall through to CXL path.
            leaveEpoch(worker_id);
            return false;
        }

        snap_value_length = entry->value_length;
        snap_worker_id = entry->last_modified_worker_id;
        snap_tx_ts = entry->tx_ts;

        if (local_value_buffer.size() < snap_value_length) {
            local_value_buffer.resize(snap_value_length);
        }
        std::memcpy(local_value_buffer.data(),
                    entry->payload + entry->key_length,
                    snap_value_length);

        if (entry->retryRead(start_v)) {
            snapshot_consistent = true;
            break;
        }
        // Otherwise concurrent writer moved the version — retry.
    }

    if (!snapshot_consistent) {
        // Too much contention; let caller fall back to CXL B+Tree path.
        leaveEpoch(worker_id);
        return false;
    }

    // MVCC visibility check on the snapshot we just took.
    bool is_visible = cr::Worker::my().cc.isVisibleForMe(snap_worker_id, snap_tx_ts, false);
    if (!is_visible) {
        leaveEpoch(worker_id);
        return false;
    }

    payload_callback(local_value_buffer.data(), snap_value_length);
    SieveFIFOQueue::OnSieveFIFOAccess(entry);
    leaveEpoch(worker_id);
    return true;
}

//===================================[Added].==========================================================
//                      Update Interceptor: tryWriteThroughSync
//
// Called AFTER the BTree update callback has written the new value to the CXL Page.
// Protocol:
//   1. enterEpoch
//   2. find entry; if not present or not in WriteThroughMode, exit (no-op)
//   3. beginWrite()  → seqlock to odd
//   4. double-check Type still 001 (else roll seqlock back to even and exit)
//   5. memcpy new_value, refresh tx_ts / worker_id / swip / slot_id
//   6. endWrite()    → seqlock to even
//   7. leaveEpoch
//
// Invariants:
//   - This function assumes new_value.size() == entry->value_length. Variable-length
//     updates (e.g., FAT_TUPLE) must NOT call this — they should call
//     tryInvalidateForRemove instead.
//   - The shard's hash table lock is NOT taken: we only read entry pointer (which
//     is kept alive by enterEpoch) and use SeqLock for field-level synchronization.
//======================================================================================================
bool RecordCache::tryWriteThroughSync(std::span<const u8> key,
                                      std::span<const u8> new_value,
                                      BufferFrame* bf,
                                      u16 slot_id,
                                      u16 worker_id,
                                      u64 tx_ts)
{
    enterEpoch(worker_id);

    RecordCacheEntry* entry = GetFromRecordCache(key);
    if (entry == nullptr) {
        leaveEpoch(worker_id);
        return false;
    }

    if (!entry->isExpectedType(RecordCacheType::WriteThroughMode)) {
        leaveEpoch(worker_id);
        return false;
    }

    if (new_value.size() != entry->value_length) {
        // Length mismatch — caller should have routed through invalidate.
        leaveEpoch(worker_id);
        return false;
    }

    entry->beginWrite();

    // Double-check Type after marking odd: a concurrent SIEVE/remove could have
    // unlinked us between the early check and the seqlock bump.
    if (!entry->isExpectedType(RecordCacheType::WriteThroughMode)) {
        entry->endWrite();
        leaveEpoch(worker_id);
        return false;
    }

    std::memcpy(entry->payload + entry->key_length,
                new_value.data(),
                entry->value_length);
    entry->tx_ts = tx_ts;
    entry->last_modified_worker_id = worker_id;
    entry->swip = Swip<BufferFrame>(bf);
    entry->slot_id = slot_id;

    entry->endWrite();
    leaveEpoch(worker_id);
    return true;
}

//===================================[Added].==========================================================
//                      Remove Interceptor: tryInvalidateForRemove
//
// Called when the BTree is about to delete a record. We:
//   1. CAS Type 001 → 100 (unlink-in-progress)
//   2. Erase from hash table under shard exclusive lock
//   3. Hand the slab chunk to ForwardEpoch via InvalidationQueue (epoch-safe free)
//
// The entry remains in the SIEVE FIFO; SIEVE will pop it later, see Type=100, and
// also push to InvalidationQueue (idempotent free path is safe via FIFO ordering
// since we only deallocate once after epoch drain).
//======================================================================================================
bool RecordCache::tryInvalidateForRemove(std::span<const u8> key, u64 worker_id)
{
    enterEpoch(worker_id);

    RecordCacheEntry* entry = GetFromRecordCache(key);
    if (entry == nullptr) {
        leaveEpoch(worker_id);
        return false;
    }

    RecordCacheType expected = RecordCacheType::WriteThroughMode;
    if (!entry->casType(expected, RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation)) {
        // Already 100 / 111 — nothing to do here.
        leaveEpoch(worker_id);
        return false;
    }

    // Unlink from hash table. EraseFromRecordCache takes the shard exclusive lock.
    EraseFromRecordCache(key);
    addToInvalidationQueue(entry, getCurrentEpoch());

    leaveEpoch(worker_id);
    return true;
}

}  // namespace recordcache
}  // namespace storage
}  // namespace leanstore
