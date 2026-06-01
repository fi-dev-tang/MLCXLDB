#pragma once
#include"../RecordCacheEntry.hpp"
#include"../RecordCache.hpp"
#include <mutex>
#include<vector>
#include<functional>

namespace leanstore{
namespace storage{
namespace recordcache{

//--------------------------------[Added].---------------------------------------------
// Core structure for SIEVE Eviction Thread
class SieveFIFOQueue{
public:
    SieveFIFOQueue() = default;

//====================================================================================================
//                     Core Operation Logic
//
// eraseElementAtHandPosition() -> Return pointer to evictOneFromSieveFIFO()
// evictOneFromSieveFIFO() -> onVictimChosen
// 
// eraseElementAtHandPosition() is only responsible for removing the pointer from the SIEVE queue,
// it does not handle physical memory deallocation, which is left to SIEVEEvictionThread to execute.
//====================================================================================================
    void InsertIntoSieveFIFO(RecordCacheEntry* entry){
        if(entry == nullptr) return;
        std::lock_guard<std::mutex> lock(sieve_fifo_mutex);

        // visited = false
        entry -> visited.store(false, std::memory_order_release);
        sieve_fifo_queue.push_back(entry);
    }

    void ReInsertIntoSieveFIFO(RecordCacheEntry* entry){
        if(entry == nullptr) return;
        std::lock_guard<std::mutex> lock(sieve_fifo_mutex);
        sieve_fifo_queue.push_back(entry);
    }

    static inline void OnSieveFIFOAccess(RecordCacheEntry* entry){
        if(entry == nullptr) return;

        // Once Accessed
        // visited = true
        entry -> visited.store(true, std::memory_order_release);
    }

    // Core Operation:
    // Attempt to evict an item.
    // if ready, return RecordCacheEntry* pointer, if failed, return nullptr
    //
    // [B-mover] shouldForceEvict: optional lambda. When it returns true for an
    // entry, the second-chance is suppressed — even visited=true entries proceed
    // directly to tryMarkEvictable evaluation. Used by SIEVE to bias drainage
    // toward under-utilized slabs so they can be reclaimed cross-class.
    RecordCacheEntry* evictOneFromSieveFIFO(
        const std::function<bool(RecordCacheEntry*)> &tryMarkEvictable,
        const std::function<void(RecordCacheEntry*)> &onVictimChosen = {},
        const std::function<bool(RecordCacheEntry*)> &shouldForceEvict = {}
    ){
        std::lock_guard<std::mutex> lock(sieve_fifo_mutex);
        if(sieve_fifo_queue.empty()) return nullptr;

        // Traverse the sieve_fifo_queue
        size_t scanned_item_num = 0;
        // Traverse twice,
        // [First Round]: True -> False,
        // [Second Round]: False -> Evict,
        const size_t max_scan_num = sieve_fifo_queue.size() * 2;

        while(scanned_item_num < max_scan_num && !sieve_fifo_queue.empty()){
            if(hand_pointer >= sieve_fifo_queue.size()){
                hand_pointer = 0;           // Guarantee that hand_pointer stays within bounds.
            }

            RecordCacheEntry* current_entry = sieve_fifo_queue[hand_pointer];

            if(current_entry == nullptr){
                eraseElementAtHandPosition();
                scanned_item_num++;
                continue;
            }

            // [B-mover] Compute force-evict before exchanging visited so we can
            // honor it even if the entry was hot. Cheap atomic read on slab
            // metadata; no extra locks.
            const bool force_evict = shouldForceEvict && shouldForceEvict(current_entry);

            // Second chance:
            // visited == true --> Skip, visited == false (Given Second-Chance)
            // [B-mover] If force_evict, clear visited but do NOT grant the second
            // chance — fall through to tryMarkEvictable so this entry can be
            // drained out of its under-utilized slab.
            const bool was_visited = current_entry -> visited.exchange(false, std::memory_order_acq_rel);
            if(was_visited && !force_evict){
                // Given Second chance, hand pointer move forward
                advanceHandPointer();
                scanned_item_num++;
                continue;
            }

            // visited == false, chosen to be victim.
            // The upper caller decide whether it can be evicted
            if(tryMarkEvictable && !tryMarkEvictable(current_entry)){
                // The current_entry can not be evicted, because of the upper RecordCacheType state
                // Skip, can not evict.
                
                // Pop from current Position, and insert into tail
                eraseElementAtHandPosition();
                sieve_fifo_queue.push_back(current_entry);
                scanned_item_num++;
                continue;
            }

            // Chosen a victim: Evict from FIFO Queue
            RecordCacheEntry *evict_victim = current_entry;
            eraseElementAtHandPosition();   

            if(onVictimChosen) onVictimChosen(evict_victim);
            return evict_victim;
        }

        return nullptr;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(sieve_fifo_mutex);
        return sieve_fifo_queue.size();
    }

    // [FIX-C] drainStateRemovedOnly:
    // Walk the FIFO once and pop up to `limit` entries currently at state
    // RemovedFromHashTableButWaitForPhysicalMemoryDeallocation (0b100).
    // Entries in any other state (including WriteThroughMode 0b001) are KEPT in
    // their original FIFO order, with their visited bits LEFT UNTOUCHED.
    //
    // Caller (SIEVE dead_pile path) only removes the pointers from the FIFO —
    // it does NOT deallocate. In WT mode the corresponding slab chunks are
    // already queued on InvalidationQueue and will be freed by ForwardEpoch
    // under epoch safety. The point of this drain is solely to clear FIFO
    // residue so the SIEVE FIFO does not grow unbounded with type=100
    // tombstones, and so subsequent watermark-driven SIEVE passes don't waste
    // work re-scanning dead entries.
    //
    // Returns the number of victims appended to `victims`.
    size_t drainStateRemovedOnly(
        std::vector<RecordCacheEntry*>& victims,
        size_t limit
    ){
        std::lock_guard<std::mutex> lock(sieve_fifo_mutex);
        if(sieve_fifo_queue.empty()) return 0;

        std::vector<RecordCacheEntry*> kept;
        kept.reserve(sieve_fifo_queue.size());
        size_t found = 0;

        for(RecordCacheEntry* entry : sieve_fifo_queue){
            if(entry == nullptr) continue; // drop stale null slots
            if(found < limit){
                auto type = entry->entry_type.load(std::memory_order_acquire);
                if(type == RecordCacheType::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation){
                    victims.push_back(entry);
                    ++found;
                    continue; // do NOT keep
                }
            }
            kept.push_back(entry);
        }

        sieve_fifo_queue = std::move(kept);
        if(hand_pointer >= sieve_fifo_queue.size()){
            hand_pointer = 0;
        }
        return found;
    }

    // [B-mover v5] Called by RecordCache::tryRescueSlabForAllocator after it has
    // unlinked target-slab entries from the hashtable. We replace those entries'
    // pointers with nullptr in the FIFO so the SIEVE thread won't later pop them
    // and double-queue them to InvalidationQueue (which would cause a double-free
    // when ForwardEpoch processes the slab). evictOneFromSieveFIFO already erases
    // null pointers it encounters during traversal, so the null slots are
    // self-cleaning.
    size_t markSlabEntriesAsNull(const char* slab_base, const char* slab_end){
        std::lock_guard<std::mutex> lock(sieve_fifo_mutex);
        size_t marked = 0;
        for(auto*& p : sieve_fifo_queue){
            if(p == nullptr) continue;
            const char* cp = reinterpret_cast<const char*>(p);
            if(cp >= slab_base && cp < slab_end){
                p = nullptr;
                marked++;
            }
        }
        return marked;
    }

private:
//=======================================================================================
//                     Hand Pointer Moving Logic
//=======================================================================================
    //------------------------------------------------------------------------------------
    // Helper function:
    // Remove sieve_fifo_queue's element at hand position.
    // Also guarantee that hand points to the next element.
    // [Note]: This function ONLY removes the pointer from the FIFO queue.
    // Physical memory deallocation (SlabAllocator) and Hash Table cleanup 
    // are intentionally NOT handled here. They are delegated to the upper-level 
    // Eviction Thread to ensure separation of concerns and avoid lock ordering issues.
    //-------------------------------------------------------------------------------------
    void eraseElementAtHandPosition(){
        if(sieve_fifo_queue.empty()){
            hand_pointer = 0;
            return;
        }

        sieve_fifo_queue.erase(sieve_fifo_queue.begin() + hand_pointer);
        if(sieve_fifo_queue.empty()){
            hand_pointer = 0;
        }else if(hand_pointer >= sieve_fifo_queue.size()){
            hand_pointer = 0;
        }
    }

    //----------------------------------------------------------------------------------------
    // Helper function:
    // Advance hand pointer.
    //----------------------------------------------------------------------------------------
    void advanceHandPointer(){
        if(sieve_fifo_queue.empty()){
            hand_pointer = 0;
            return;
        }
        else{
            hand_pointer = (hand_pointer + 1) % sieve_fifo_queue.size();
        }
    }

private:
    mutable std::mutex sieve_fifo_mutex;
    std::vector<RecordCacheEntry*> sieve_fifo_queue;
    size_t hand_pointer = 0;                // SIEVE Algorithm's hand pointer
};
}
}
}