#include"../RecordCache.hpp"
#include"../RecordCacheSlabAllocator.hpp"
#include"SieveFIFO.hpp"
#include<chrono>
#include<pthread.h>
#include<thread>

namespace leanstore{
namespace storage{
namespace recordcache{

//====================================[Added].===============================================
//          Core Eviction Logic (SIEVE with Second Chance via FIFO)
//
// "hand" pointer movement is implemented as FIFO pop + conditional reinsert:
//   - pop_front()      : hand moves to the oldest entry
//   - reinsert_back()  : entry gets a second chance, hand skips it
//   - evict()          : hand evicts the entry
//
// Decision flow for each popped entry:
//
// [Step 1] Pop entry from SieveFIFOQueue head
//
// [Step 2] Check entry_type:
//
//   (2.1) Type = WriteThroughMode (0b001)
//         → Check visited bit:
//             visited = true  : Clear visited bit, reinsert_back (second chance)
//             visited = false : Proceed to eviction:
//                               ① EraseFromRecordCache  (remove from hashtable)
//                               ② setType(100) and push to InvalidationQueue
//                                  (epoch-safe slab free is handled by ForwardEpoch)
//
//   (2.2) Type = PromoteThreadHoldingThePosition (0b111)
//         → Entry is in transitional state, not safe to evict yet
//           reinsert_back, wait for Promote thread to finish
//
//   (2.3) Type = RemovedFromHashTableButWaitForPhysicalMemoryDeallocation (0b100)
//         → Already removed from hashtable; deallocate directly
//           (this case fires when a Promote attempt aborted leaving the entry
//            in 100 state in the FIFO, with no hash table reference and no
//            outstanding readers)
//     
//===========================================================================================
void RecordCache::sieveEvictionThread()
{
    std::string thread_name("rc_sieve_eviction");
    pthread_setname_np(pthread_self(), thread_name.c_str());

    // NOTE:
    // - This callback runs under FIFO queue mutex.
    // - Keep it lock-free (atomic loads only), no shard/hash locks.
    auto tryMarkEvictable = [](RecordCacheEntry* entry) -> bool {
        if (entry == nullptr) return false;
        auto type = entry->entry_type.load(std::memory_order_acquire);

        // Only these two states can be popped as candidates:
        // 001: needs hash-table unlink before delayed free
        // 100: already unlinked, can free directly
        return (type == RecordCacheType::WriteThroughMode) ||
               (type == RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation);
    };

    // [B-mover] Bias SIEVE toward draining slabs whose per-slab utilization sits
    // ≥15 percentage points below the current global utilization. Entries on such
    // "below-average" slabs lose their second chance so the slab empties faster
    // and can be reclaimed cross-class by the allocator. Relative threshold —
    // a fixed 20% absolute would never fire when the cache is steady at 90%.
    // Cheap: two relaxed atomic loads + one ratio, no extra locks.
    auto shouldForceEvict = [this](RecordCacheEntry* entry) -> bool {
        if (entry == nullptr) return false;
        return allocator.isSlabUnderUtilized(entry, 0.15);
    };

    // [FIX-C] Tunables for the invalidation fast-path:
    //   kSieveBatchLimit          : how many FIFO entries to drain per wake
    //                               before yielding, to bound mutex hold time.
    //   kPendingDeadEntryThreshold: SIEVE wakes proactively when this many
    //                               type=100 entries are sitting in the FIFO
    //                               (i.e. invalidated by a worker / failed
    //                               PromoteThread Phase 2 / rescue-pushed-to-IQ
    //                               and now occupying slab blocks). Triggers
    //                               without waiting for the 0.90 watermark.
    constexpr size_t kSieveBatchLimit          = 32;
    constexpr u64    kPendingDeadEntryThreshold = 1024;

    while (bg_threads_keep_running.load(std::memory_order_acquire)) {
        const bool slab_pressure = allocator.getUsageRatio() >= EvictionWaterMark;
        const bool dead_pile     = pending_state_invalidated_from_hash.load(std::memory_order_relaxed)
                                   >= kPendingDeadEntryThreshold;
        if (!slab_pressure && !dead_pile) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // ============================================================
        // [FIX-C] Path 1: dead_pile only (slab NOT under pressure)
        // ============================================================
        // The cache is not at watermark, but worker invalidation has piled up
        // type=100 tombstones in the FIFO that are still occupying slab blocks.
        // Drain them out of the FIFO so the pointer slots are reclaimed; the
        // actual slab chunk free is owned by ForwardEpoch (the entries are
        // already on InvalidationQueue from the invalidate / Phase2-fail path).
        //
        // CRITICAL: do NOT touch any WriteThroughMode entry's visited bit here.
        // The second-chance signal of real hot keys must be preserved when the
        // cache is below watermark.
        if (!slab_pressure /* && dead_pile */) {
            std::vector<RecordCacheEntry*> dead_victims;
            dead_victims.reserve(kSieveBatchLimit);
            const size_t got = sieve_fifo_queue.drainStateRemovedOnly(
                dead_victims, kSieveBatchLimit);

            for (size_t i = 0; i < got; ++i) {
                // Pointer is already in InvalidationQueue; ForwardEpoch will
                // dealloc under epoch safety. Here we only mark FIFO drain
                // accounting + decrement the dead-pile counter.
                DecrementPendingStateInvalidatedFromHash();
                sieve_eviction_entries.fetch_add(1, std::memory_order_relaxed);
            }

            if (got == 0) {
                // Counter said dead_pile but FIFO had nothing at type=100
                // (transient race) — short sleep so we don't busy-loop.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            continue;
        }

        // ============================================================
        // [FIX-C] Path 2: slab_pressure (real cache full)
        // ============================================================
        // Use the original SIEVE flow (visited-bit second chance, may evict
        // WriteThrough entries). Batch up to kSieveBatchLimit per wake to keep
        // mutex hold time bounded.
        bool nothing_evictable = false;
        for (size_t batch_count = 0; batch_count < kSieveBatchLimit; ++batch_count) {
            if (!bg_threads_keep_running.load(std::memory_order_acquire)) break;

            RecordCacheEntry* victim = sieve_fifo_queue.evictOneFromSieveFIFO(
                tryMarkEvictable, /*onVictimChosen=*/{}, shouldForceEvict);
            if (!victim) { nothing_evictable = true; break; }

            auto type = victim->entry_type.load(std::memory_order_acquire);

            // ------------------------------------------------------------
            // Case A: 100 -> already removed from hash table.
            // The pointer was already on InvalidationQueue when the producer
            // (worker invalidate / PromoteThread Phase 2 fail / rescue) set
            // type=100, so we do NOT push it again — double-push would cause
            // ForwardEpoch to double-free. We only need to account the drain.
            // ------------------------------------------------------------
            if (type == RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation) {
                DecrementPendingStateInvalidatedFromHash();
                sieve_eviction_entries.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // ------------------------------------------------------------------
            // Case B: 001 -> SIEVE itself unlinks + transitions to 100 + queues
            // to InvalidationQueue. This entry was never counted in the dead-pile
            // counter (it was a stable WriteThrough until just now), so we do
            // NOT decrement the counter here.
            // -------------------------------------------------------------------
            if (type == RecordCacheType::WriteThroughMode) {
                const u64 hash_value = HashBytes(victim->getKeySpan());
                auto& target_shard = getShardByHash(hash_value);

                std::unique_lock<std::shared_mutex> lock(target_shard.mutex);

                // Double-check under shard lock: only handle stable 001 here.
                if (victim->entry_type.load(std::memory_order_acquire) != RecordCacheType::WriteThroughMode) {
                    lock.unlock();
                    sieve_fifo_queue.ReInsertIntoSieveFIFO(victim);
                    continue;
                }

                // Key + pointer match to avoid removing a newer replacement.
                std::string key_str(
                    reinterpret_cast<const char*>(victim->getKeySpan().data()),
                    victim->getKeySpan().size()
                );

                auto it = target_shard.hash_map_shard.find(key_str);
                if (it != target_shard.hash_map_shard.end() && it->second == victim) {
                    target_shard.hash_map_shard.erase(it);
                    active_entry_count.fetch_sub(1, std::memory_order_relaxed);

                    victim->setType(RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation);

                    lock.unlock();
                    // Defer slab free to ForwardEpoch: a concurrent SeqLock reader
                    // may still be holding a snapshot pointer to victim.
                    addToInvalidationQueue(victim, getCurrentEpoch());
                    sieve_eviction_entries.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                // Not found or pointer mismatch: ownership not proven, do not free.
                lock.unlock();
                sieve_fifo_queue.ReInsertIntoSieveFIFO(victim);
                continue;
            }

            // ------------------------------------------------------------
            // Case C: transitional state 111 (Promote in progress) or unexpected
            // ------------------------------------------------------------
            sieve_fifo_queue.ReInsertIntoSieveFIFO(victim);
        } // end batch loop

        if (nothing_evictable) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bg_threads_counter.fetch_sub(1, std::memory_order_release);
}
}    
}  
}