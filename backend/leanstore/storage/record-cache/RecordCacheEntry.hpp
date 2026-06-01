# pragma once
#include<atomic>
#include<span>

#include "Units.hpp"
#include "../buffer-manager/Swip.hpp"

namespace leanstore{
namespace storage{
namespace recordcache{

//=========================================================================================================================================================
//      RecordCache Type State Machine (Write-Through)
//=========================================================================================================================================================
// 001: WriteThroughMode      — active, readable entry (the only stable hit state in WT)
// 100: RemovedFromHashTable… — already unlinked from hash table, awaiting slab free
// 111: PromoteThread holding the position — placeholder during promote
//
// Read-Only `000` and Logical-Delete `011` are intentionally absent: in WT mode
// updates do not deletion the cache entry; concurrency is handled by SeqLock +
// double-check on Type instead of the InvalidationQueue two-phase delete path.
enum class RecordCacheType: u8 {
    WriteThroughMode                                            = 0b001,        // 001: Write-Through mode, active
    RemovedFromHashTableButWaitForPhysicalMemoryDeallocation    = 0b100,        // 100: Removed from hash table, wait for slab free
    PromoteThreadHoldingThePosition                             = 0b111,        // 111: Promote thread occupying the state
};

//---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Write-Through RecordCacheEntry layout:
//
// [ Meta Data         : type | key_length | value_length ]
// [ Eviction Control  : visited ]
// [ Visibility Check  : last_modified_worker_id | tx_ts ]
// [ Quick Index to CXL: swip | slot_id ]
// [ Lock              : seqlock ]
// [ Raw Data          : key | value ]
struct RecordCacheEntry{
    //--------------------[Visibility Check] ----------------------------------------
    // Transaction ID/timestamp of the last modification.
    // Highest bit indicates if transaction is committed.
    u64 tx_ts;

    // The MVCC component compatible with legacy logic.
    u16 last_modified_worker_id;

    //--------------------[Meta Data] -----------------------------------------------
    u16 key_length;
    u16 value_length;

    std::atomic<RecordCacheType> entry_type;                    // default: WriteThroughMode unset; PromoteThread sets initial value

    //--------------------[Eviction Control] ----------------------------------------
    // 1-bit atomic visited flag for SIEVE eviction logic.
    std::atomic<bool> visited;

    //--------------------[Quick Index to CXL] --------------------------------------
    // Swip pointing to the BufferFrame holding the source page; usable for
    // fast path probes during update without re-walking the B+Tree.
    Swip<BufferFrame> swip;

    // Slot offset of the record on the source page. Combined with swip,
    // worker can do (BTreeNode*)bf->page.dt->slot[slot_id] in one indirection.
    u16 slot_id;

    //--------------------[SeqLock] -------------------------------------------------
    // Optimistic version: even = stable, odd = writer in progress.
    // Reader: snapshot start, copy fields, retry until end matches start.
    // Writer: fetch_add(1) → odd → mutate → fetch_add(1) → even.
    std::atomic<u64> seqlock;

    //--------------------[Raw Data] ------------------------------------------------
    // Memory layout: [Key bytes ...] [Value bytes ...]
    u8 payload[];

    //==============================================================================
    //                  Inline Accessors
    //==============================================================================
    inline std::span<u8> getKeySpan(){
        return {payload, key_length};
    }

    inline std::span<const u8> getKeySpan() const {
        return {payload, key_length};
    }

    inline std::span<u8> getValueSpan(){
        return {payload + key_length, value_length};
    }

    inline std::span<const u8> getValueSpan() const{
        return {payload + key_length, value_length};
    }

    inline size_t totalSizeForRecordCacheEntry(){
        return sizeof(RecordCacheEntry) + key_length + value_length;
    }

    //===============================================================================
    //                  Type State Machine Helpers
    //===============================================================================
    inline bool isExpectedType(RecordCacheType expected_type) const {
        return entry_type.load(std::memory_order_acquire) == expected_type;
    }

    inline void setType(RecordCacheType new_type){
        entry_type.store(new_type, std::memory_order_release);
    }

    inline bool casType(RecordCacheType& expected_type, RecordCacheType new_type) {
        return entry_type.compare_exchange_strong(expected_type, new_type, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    //===============================================================================
    //                  SeqLock Helpers
    //===============================================================================
    // Reader: spin until version is even, then return that snapshot.
    inline u64 beginRead() const {
        for(;;){
            u64 v = seqlock.load(std::memory_order_acquire);
            if((v & 1ULL) == 0) return v;
            // Odd: writer in progress; spin/yield.
            __builtin_ia32_pause();
        }
    }

    // Reader: returns true if the snapshot is consistent (version did not move).
    inline bool retryRead(u64 start_version) const {
        std::atomic_thread_fence(std::memory_order_acquire);
        return seqlock.load(std::memory_order_acquire) == start_version;
    }

    // Writer: enter critical section (version becomes odd).
    inline void beginWrite(){
        seqlock.fetch_add(1, std::memory_order_acq_rel);
    }

    // Writer: leave critical section (version becomes even).
    inline void endWrite(){
        seqlock.fetch_add(1, std::memory_order_acq_rel);
    }
};
}
}
}
