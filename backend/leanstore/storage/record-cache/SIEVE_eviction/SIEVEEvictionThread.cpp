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

    while (bg_threads_keep_running.load(std::memory_order_acquire)) {
        // Watermark check: only evict when usage is above LowWatermarkRatio
        if (allocator.getUsageRatio() < EvictionWaterMark) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        RecordCacheEntry* victim = sieve_fifo_queue.evictOneFromSieveFIFO(tryMarkEvictable);
        if (!victim) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto type = victim->entry_type.load(std::memory_order_acquire);

        // ------------------------------------------------------------
        // Case A: 100 -> already removed from hash table.
        // Hand off to ForwardEpoch for epoch-safe slab free; a Lookup that
        // grabbed the pointer just before unlinking may still be holding it.
        // ------------------------------------------------------------
        if (type == RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation) {
            addToInvalidationQueue(victim, getCurrentEpoch());
            sieve_eviction_entries.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // ------------------------------------------------------------------
        // Case B: 001 -> unlink from hash table under shard lock, then hand off
        //                to ForwardEpoch for epoch-safe slab deallocation.
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
    }

    bg_threads_counter.fetch_sub(1, std::memory_order_release);
}
}    
}  
}