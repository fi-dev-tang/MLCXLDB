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
        // [B-mover v5] Wire allocator → RecordCache rescue. Must happen before
        // any promote/eviction thread spins up, because PromoteThread may call
        // allocator.allocate (which now expects the callback to be live).
        allocator.setSlabRescueCallback([this]() {
            return tryRescueSlabForAllocator();
        });

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
bool RecordCache::tryLookupInRecordCache(u16 dt_id, std::span<const u8> key,
        const std::function<void(const u8*, u16)>& payload_callback, u64 worker_id)
{
    enterEpoch(worker_id);

    // [v4-a] Build (dt_id || key) on the stack so tables with byte-identical folded
    // keys (TPC-C customer/order/neworder all use Integer w,d,id → 12B) do not
    // collide. With FLAGS_rc_skip_dt_id_prefix=true (single-table workloads only),
    // BuildPrefixedKey returns the original key span and skips the memcpy.
    constexpr size_t kMaxPrefixedKey = 128;
    assert(key.size() + 2 <= kMaxPrefixedKey);
    u8 prefixed_buf[kMaxPrefixedKey];
    std::span<const u8> prefixed_key = BuildPrefixedKey(dt_id, key, prefixed_buf);

    RecordCacheEntry* entry = GetFromRecordCache(prefixed_key);
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

    // [v4-a] Bound the SeqLock retry loop so we never spin forever on a livelock.
    // Tunable via --rc_seqlock_retry_max (default 8). Lower = give up to CXL path
    // sooner under writer contention; higher = squeeze every retry first.
    const u32 kMaxSeqRetry = FLAGS_rc_seqlock_retry_max;
    bool snapshot_consistent = false;
    for (u32 attempt = 0; attempt < kMaxSeqRetry; ++attempt) {
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
bool RecordCache::tryWriteThroughSync(u16 dt_id,
                                      std::span<const u8> key,
                                      std::span<const u8> new_value,
                                      BufferFrame* bf,
                                      u16 slot_id,
                                      u16 worker_id,
                                      u64 tx_ts)
{
    enterEpoch(worker_id);

    constexpr size_t kMaxPrefixedKey = 128;
    assert(key.size() + 2 <= kMaxPrefixedKey);
    u8 prefixed_buf[kMaxPrefixedKey];
    std::span<const u8> prefixed_key = BuildPrefixedKey(dt_id, key, prefixed_buf);

    RecordCacheEntry* entry = GetFromRecordCache(prefixed_key);
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
bool RecordCache::tryInvalidateForRemove(u16 dt_id, std::span<const u8> key, u64 worker_id)
{
    enterEpoch(worker_id);

    constexpr size_t kMaxPrefixedKey = 128;
    assert(key.size() + 2 <= kMaxPrefixedKey);
    u8 prefixed_buf[kMaxPrefixedKey];
    std::span<const u8> prefixed_key = BuildPrefixedKey(dt_id, key, prefixed_buf);

    RecordCacheEntry* entry = GetFromRecordCache(prefixed_key);
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
    EraseFromRecordCache(prefixed_key);
    addToInvalidationQueue(entry, getCurrentEpoch());
    // [FIX-C] Entry is now type=100 sitting in InvalidationQueue. It is also
    // still in the SIEVE FIFO holding a slab block. Bump the counter so SIEVE
    // wakes via the dead_pile path and drains the FIFO residue.
    IncrementPendingStateInvalidatedFromHash();

    leaveEpoch(worker_id);
    return true;
}

//===================================[Added].==========================================================
//                      Slab Rescue: B-mover v5
//
// Called by RecordCacheSlabAllocator::do_allocate when it cannot get a slab via
// pool reuse nor carve. Synchronously drains entries on a low-live slab and
// returns it to the pool. See RecordCache.hpp for the algorithm summary.
//
// Safety story:
//   - Only PromoteThread calls allocator.allocate (verified by grep). PromoteThread
//     is NOT epoch-protected, so rescuing from it does not block its own epoch.
//   - Hash table iteration takes each shard's unique_lock; entries in transit
//     (type=111 promote-held, or type=100 already unlinked but still hash-resident
//     for a moment) are NOT touched — only stable 001 entries are unlinked.
//   - SIEVE FIFO is updated under its own mutex so the SIEVE thread can run
//     concurrently without seeing stale pointers.
//   - We capture the global epoch AFTER unlinking, then spin until all workers
//     active at that epoch have left or moved forward. ForwardEpoch advances
//     the global epoch once per ms; we also advance once ourselves to keep the
//     wait short.
//======================================================================================================
bool RecordCache::tryRescueSlabForAllocator()
{
    // Serialize rescuers: there's no benefit to multiple workers iterating the
    // hash table simultaneously, and double-deallocate is a hard error.
    std::lock_guard<std::mutex> rescue_lock(slab_rescue_mutex);

    // Short-circuit: a previous rescuer (or a normal SIEVE drain that completed
    // while we were queueing on the mutex) may already have populated the pool.
    if (allocator.hasFreeSlab()) {
        return true;
    }

    // Try the few lowest-live slabs first. Five is empirical: a slab whose
    // entries are all 111 (PromoteThread holding) yields zero unlinks and we
    // must move to the next candidate.
    constexpr size_t kMaxCandidates = 5;
    auto candidates = allocator.findRescueCandidates(kMaxCandidates);
    if (candidates.empty()) {
        return false;
    }

    for (const auto& cand : candidates) {
        const char* slab_base = cand.slab_base;
        const char* slab_end  = cand.slab_base + cand.slab_bytes;

        // Phase 1: unlink every type=001 entry on this slab from the hash table.
        // CAS 001 → 100 atomically with the erase so a concurrent reader either
        // sees the entry (type still 001, fetches the value) or doesn't find it
        // in the hash table at all (we erased after CAS).
        std::vector<RecordCacheEntry*> collected;
        collected.reserve(cand.live_count);

        for (size_t s = 0; s < num_of_shards; s++) {
            auto& shard = hash_shards[s];
            std::unique_lock<std::shared_mutex> shard_lock(shard.mutex);
            for (auto it = shard.hash_map_shard.begin(); it != shard.hash_map_shard.end(); ) {
                RecordCacheEntry* entry = it->second;
                const char* cp = reinterpret_cast<const char*>(entry);
                if (cp >= slab_base && cp < slab_end) {
                    RecordCacheType expected = RecordCacheType::WriteThroughMode;
                    if (entry->casType(expected,
                            RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation)) {
                        it = shard.hash_map_shard.erase(it);
                        active_entry_count.fetch_sub(1, std::memory_order_relaxed);
                        collected.push_back(entry);
                        continue;
                    }
                    // type was 111 (promote in progress) or 100 (already unlinked
                    // but somehow still in map — shouldn't happen but is benign).
                    // Leave the map intact and skip.
                }
                ++it;
            }
        }

        if (collected.empty()) {
            // Pure 111 slab — try next candidate.
            continue;
        }

        // Phase 2: nullify these entries in the SIEVE FIFO so the SIEVE thread
        // doesn't pop them later and re-queue them to InvalidationQueue (which
        // would cause a double-free when ForwardEpoch processes the slab).
        sieve_fifo_queue.markSlabEntriesAsNull(slab_base, slab_end);

        // Phase 3: wait for epoch safety. Capture e_unlink AFTER unlinking so
        // any worker that snapshotted entry pointers BEFORE the unlink has
        // current_epoch ≤ e_unlink. Then advance global epoch once so any
        // worker that enters AFTER advance sees current_epoch > e_unlink and
        // never blocks the is_safe check.
        const u64 e_unlink = epoch_manager.get_global_epoch();
        epoch_manager.periodically_advance_global_epoch();

        // Bound the wait: 100 ms at 100 µs poll. In practice workers
        // enter/leave RecordCache in microseconds, so this usually returns
        // after one or two iterations.
        constexpr int kMaxPoll = 1000;
        bool safe = false;
        for (int i = 0; i < kMaxPoll; i++) {
            if (epoch_manager.is_safe_to_invalidate(e_unlink)) {
                safe = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        if (!safe) {
            // Some worker is stuck in a long-running RecordCache op. Don't
            // deallocate (would risk UAF); push the collected entries onto the
            // normal InvalidationQueue so ForwardEpoch eventually handles them.
            // Our slab won't reach live=0 inside this call, so report failure
            // and let the next rescue attempt pick a different target.
            for (auto* entry : collected) {
                addToInvalidationQueue(entry, e_unlink);
            }
            fprintf(stderr,
                "[N4-DIAG][SLAB-RESCUE] timeout waiting for epoch slab_idx=%zu unlinked=%zu — handed off to InvalidationQueue\n",
                cand.slab_idx, collected.size());
            fflush(stderr);
            continue;
        }

        // Phase 4: inline deallocate. Each call decrements live_count for the
        // entry's slab; the last one drives our target slab's live_count to 0
        // and triggers tryReclaimSlab from inside do_deallocate.
        for (auto* entry : collected) {
            const size_t entry_size = entry->totalSizeForRecordCacheEntry();
            allocator.deallocate(entry, entry_size, alignof(RecordCacheEntry));
        }

        // Check if the pool now has a slab. It may not, if the target had
        // any 111 entries we couldn't touch — those still hold the slab open.
        if (allocator.hasFreeSlab()) {
            fprintf(stderr,
                "[N4-DIAG][SLAB-RESCUE] success slab_idx=%zu block_size=%zu unlinked=%zu\n",
                cand.slab_idx, cand.block_size, collected.size());
            fflush(stderr);
            return true;
        }
        // Pool still empty: target had outstanding promote-held entries. Move
        // on to the next candidate.
    }

    return allocator.hasFreeSlab();
}

}  // namespace recordcache
}  // namespace storage
}  // namespace leanstore
