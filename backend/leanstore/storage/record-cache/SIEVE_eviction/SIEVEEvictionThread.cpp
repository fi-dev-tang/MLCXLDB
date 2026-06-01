#include"../RecordCache.hpp"
#include"../RecordCacheSlabAllocator.hpp"
#include"SieveFIFO.hpp"
#include<chrono>
#include<pthread.h>
#include<string>
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
//   (2.1) Type = ReadOnlyMode (0b000)
//         → Check visited bit:
//             visited = true  : Clear visited bit, reinsert_back (second chance)
//             visited = false : Proceed to eviction:
//                               ① EraseFromRecordCache  (remove from hashtable)
//                               ② deallocate            (return slab chunk to free_list)
//
//   (2.2) Type = LogicallyDeletedButStillInHashTable (0b011)
//               or PromoteThreadHoldingThePosition   (0b111)
//         → Entry is in transitional state, not safe to evict yet
//           reinsert_back, wait for Forward_epoch or Promote thread to finish
//
//   (2.3) Type = RemovedFromHashTableButWaitForPhysicalMemoryDeallocation (0b100)
//         → Already removed from hashtable by Forward_epoch
//           deallocate directly (return slab chunk to free_list)
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
        // 000: needs hash-table unlink before free
        // 100: already unlinked, can free directly
        return (type == RecordCacheType::ReadOnlyMode) ||
               (type == RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation);
    };

    // [FIX-C v2] tunables:
    //   kSieveBatchLimit          : how many FIFO entries to drain per wake
    //                               before yielding (bound mutex hold time
    //                               and avoid starving worker insert path).
    //   kPendingDeadEntryThreshold : SIEVE wakes proactively once this many
    //                               state=100 entries have been queued by
    //                               forward_epoch / Phase3 / CXL-fail paths,
    //                               even if usage_ratio is below watermark.
    //                               When woken purely by this signal we only
    //                               drain state=100 (drainStateRemovedOnly),
    //                               we do NOT touch ReadOnly hot entries.
    constexpr size_t kSieveBatchLimit          = 32;
    constexpr u64    kPendingDeadEntryThreshold = 1024;

    // [FIX-C v2] inline helper: deallocate a state=100 victim and update
    // all counters in one place. Used by both the dead-pile drain path
    // and the watermark-driven Case A path so the accounting stays
    // consistent.
    auto handle_state100_victim = [&](RecordCacheEntry* victim) {
        allocator.deallocate(victim, victim->totalSizeForRecordCacheEntry(), alignof(RecordCacheEntry));
        // [FIX-A] -1 paired with PromoteThread::allocate_new_entry +1
        active_entry_count.fetch_sub(1, std::memory_order_relaxed);
        // [FIX-C] one less pending dead entry — let SIEVE wake condition relax.
        DecrementPendingStateInvalidatedFromHash();
        sieve_eviction_entries.fetch_add(1, std::memory_order_relaxed);
    };

    while (bg_threads_keep_running.load(std::memory_order_acquire)) {
        const bool slab_pressure = allocator.getUsageRatio() >= EvictionWaterMark;
        const bool dead_pile     = pending_state_invalidated_from_hash.load(std::memory_order_relaxed)
                                   >= kPendingDeadEntryThreshold;
        if (!slab_pressure && !dead_pile) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // ============================================================
        // [FIX-C v2] Path 1: dead_pile only (slab NOT under pressure)
        // ============================================================
        // Only release entries already at state=100. Do NOT touch any
        // ReadOnly entry's visited bit. This preserves second-chance
        // protection for real hot keys, which Plan C v1 broke.
        if (!slab_pressure /* && dead_pile */) {
            std::vector<RecordCacheEntry*> dead_victims;
            dead_victims.reserve(kSieveBatchLimit);
            const size_t got = sieve_fifo_queue.drainStateRemovedOnly(
                dead_victims, kSieveBatchLimit);

            for (RecordCacheEntry* victim : dead_victims) {
                handle_state100_victim(victim);
            }

            // Defensive: if pending counter said dead_pile but FIFO had
            // nothing actually at state=100 (race or transient), pause
            // briefly so we don't busy-loop.
            if (got == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            continue;
        }

        // ============================================================
        // [FIX-C v2] Path 2: slab_pressure (real cache full)
        // ============================================================
        // Use the original SIEVE flow (visited-bit second-chance + may
        // evict ReadOnly). Batch-process up to kSieveBatchLimit per wake
        // to keep mutex hold time bounded.
        bool nothing_evictable = false;
        for (size_t batch_count = 0; batch_count < kSieveBatchLimit; ++batch_count) {
            if (!bg_threads_keep_running.load(std::memory_order_acquire)) break;

            RecordCacheEntry* victim = sieve_fifo_queue.evictOneFromSieveFIFO(tryMarkEvictable);
            if (!victim) { nothing_evictable = true; break; }

            auto type = victim->entry_type.load(std::memory_order_acquire);

            // Case A: 100 -> directly free via the shared helper
            if (type == RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation) {
                handle_state100_victim(victim);
                continue;
            }

            // Case B: 000 -> shard-locked erase + free
            if (type == RecordCacheType::ReadOnlyMode) {
                const u64 hash_value = HashBytes(victim->getKeySpan());
                auto& target_shard = getShardByHash(hash_value);

                std::unique_lock<std::shared_mutex> lock(target_shard.mutex);

                if (victim->entry_type.load(std::memory_order_acquire) != RecordCacheType::ReadOnlyMode) {
                    lock.unlock();
                    sieve_fifo_queue.ReInsertIntoSieveFIFO(victim);
                    continue;
                }

                std::string key_str(
                    reinterpret_cast<const char*>(victim->getKeySpan().data()),
                    victim->getKeySpan().size()
                );

                auto it = target_shard.hash_map_shard.find(key_str);
                if (it != target_shard.hash_map_shard.end() && it->second == victim) {
                    target_shard.hash_map_shard.erase(it);

                    // NOTE: Case B does NOT decrement
                    // pending_state_invalidated_from_hash — the entry was
                    // never counted in it (transitioned 000 -> Removed by
                    // SIEVE itself, not via forward_epoch/phase3).
                    victim->setType(RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation);

                    lock.unlock();
                    allocator.deallocate(
                        victim,
                        victim->totalSizeForRecordCacheEntry(),
                        alignof(RecordCacheEntry)
                    );
                    // [FIX-A] -1 paired with allocate_new_entry +1
                    active_entry_count.fetch_sub(1, std::memory_order_relaxed);
                    sieve_eviction_entries.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                // Pointer mismatch: ownership not proven, do not free.
                lock.unlock();
                sieve_fifo_queue.ReInsertIntoSieveFIFO(victim);
                continue;
            }

            // Case C: transitional states (011/111) — leave for next pass
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