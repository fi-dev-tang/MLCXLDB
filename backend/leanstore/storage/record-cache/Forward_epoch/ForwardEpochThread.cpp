#include "../RecordCache.hpp"
#include <chrono>
#include <pthread.h>
#include <span>
#include <thread>

namespace leanstore {
namespace storage {
namespace recordcache {

void RecordCache::forwardEpochThread()
{
    std::string thread_name("rc_forward_epoch");
    pthread_setname_np(pthread_self(), thread_name.c_str());

    std::vector<InvalidationEntry> local_invalidation_retry_queue;
    local_invalidation_retry_queue.reserve(1024);

    while (bg_threads_keep_running.load(std::memory_order_acquire)) {
        // avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // [1] periodically advance global epoch
        epoch_manager.periodically_advance_global_epoch();

        // [2] Obtain local entries from InvalidationQueue
        std::vector<InvalidationEntry> this_turn_new_invalidation = invalidation_queue.pop_all();

        // [2.1] Combine new entries to local_invalidation_retry_queue
        if (!this_turn_new_invalidation.empty()) {
            local_invalidation_retry_queue.insert(local_invalidation_retry_queue.end(),
                                                  this_turn_new_invalidation.begin(),
                                                  this_turn_new_invalidation.end());
        }

        // [2.2] If there are no entries to process, continue to next iteration
        if (local_invalidation_retry_queue.empty()) {
            continue;
        }

        // [2.3] traverse all InvalidationEntry in local_invalidation_retry_queue
        // In WT mode, the producer (SIEVE / BTree remove) has already unlinked the
        // entry from the hash table and set Type=100. ForwardEpoch only needs to
        // wait for the snapshot epoch to drain, then return the slab chunk.
        std::vector<InvalidationEntry> next_turn_retry_queue;
        next_turn_retry_queue.reserve(local_invalidation_retry_queue.size());

        for (const auto& item : local_invalidation_retry_queue) {
            if (epoch_manager.is_safe_to_invalidate(item.update_epoch)) {
                RecordCacheEntry* entry_ptr = item.entry_pointer;
                allocator.deallocate(
                    entry_ptr,
                    entry_ptr->totalSizeForRecordCacheEntry(),
                    alignof(RecordCacheEntry)
                );
            } else {
                // Unsafe, there are still worker threads holding snapshot pointers.
                next_turn_retry_queue.push_back(item);
            }
        }

        // [4] Update local_invalidation_retry_queue
        local_invalidation_retry_queue = std::move(next_turn_retry_queue);
    }

    // Thread is exiting, decrement counter
    bg_threads_counter.fetch_sub(1, std::memory_order_release);
}

}  // namespace recordcache
}  // namespace storage
}  // namespace leanstore