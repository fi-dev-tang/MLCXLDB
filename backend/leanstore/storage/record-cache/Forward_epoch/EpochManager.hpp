#pragma once
#include<atomic>
#include<vector>
#include<mutex>
#include<memory>
#include"Units.hpp"
#include"../../../Config.hpp"

//--------------------------------------[Added].-------------------------------------------
// Foundation of Forward_epoch thread in RecordCacheEntry Invalidation
// Global: global_epoch
// Worker: bool active: (thread-local) (this RecordCacheEntry is still in use)
// Worker: thread_local_epoch snapshot

namespace leanstore{
namespace storage{
namespace recordcache{

// Writer: the only worker thread that ownes the thread_local_epoch
// Reader: Forward_epoch thread, check whether it can make progress.
struct alignas(64) thread_local_epoch{
    std::atomic<u64> current_epoch{0};          // snapshot of global_epoch when enter RecordCache.
    std::atomic<bool> active{false};            // if true, means RecordCacheEntry can not be invalidate, because it is still in use.
};

//--------------------------------------[Added].---------------------------------------------------
// Provide interface for 
// 1. Global: Periodically update global_epoch.
// 2. WorkerThread: Assign workerThread its current thread_local_epoch, when entering RecordCache
// 3. WorkerThread: Leave Epoch
// 4. Safe to Invalidate
class EpochManager{
public:
    explicit EpochManager(size_t total_workers_number = FLAGS_worker_threads)
    : total_workers_number(total_workers_number),
    worker_threads_epochs(std::make_unique<thread_local_epoch[]>(total_workers_number))
    {}

    // Disable Copy constuctor and copy assignment
    EpochManager(const EpochManager&) = delete;
    EpochManager& operator=(const EpochManager&) = delete;

    // Helper function
    // Global: Periodically update Global_epoch.
    // ForwardEpoch Thread calling.
    void periodically_advance_global_epoch(){
        global_epoch.fetch_add(1, std::memory_order_release);
    }

    // WorkerThread Calling:
    //  WorkerThread get its thread_local_epoch snapshot, when entering RecordCache
    void worker_thread_enter_epoch(u64 worker_id){
        // Step 1. Tag as Active
        worker_threads_epochs[worker_id].active.store(true, std::memory_order_seq_cst);

        // Step 2. Get the current global epoch
        u64 current_global_epoch = global_epoch.load(std::memory_order_acquire);
        
        // Step 3. Mark snapshot
        worker_threads_epochs[worker_id].current_epoch.store(current_global_epoch, std::memory_order_release);
    }

    // WorkerThread: Leave Epoch
    void worker_thread_leave_epoch(u64 worker_id){
        worker_threads_epochs[worker_id].active.store(false, std::memory_order_release);
    }

    // WorkerThread calling
    u64 get_global_epoch() const {
        return global_epoch.load(std::memory_order_acquire);
    }

    // Forward Epoch thread calling
    // Safe to Invalidate
    // An entry deleted at epoch E can only be physically reclaimed when all active workers
    // have progressed past epoch E (i.e., all active workers have current_epoch > E).
    bool is_safe_to_invalidate(u64 target_epoch) const{
        for(size_t i = 0; i < total_workers_number; i++){
            bool is_active = worker_threads_epochs[i].active.load(std::memory_order_acquire);
            if(is_active){
                u64 current_thread_epoch = worker_threads_epochs[i].current_epoch.load(std::memory_order_relaxed);

                // If any active worker is at or before target_epoch, it's unsafe to delete
                if(current_thread_epoch <= target_epoch){
                    return false;
                }
            }
        }
        // All active workers have progressed past target_epoch, safe to delete
        return true;
    }

private:
    alignas(64) std::atomic<u64> global_epoch{0};

    // Array of all thread_local_epoch
    std::unique_ptr<thread_local_epoch[]> worker_threads_epochs;

    // Manage worker registration
    size_t total_workers_number;
};
}    
}
}