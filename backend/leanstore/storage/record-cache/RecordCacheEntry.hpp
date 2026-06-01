# pragma once
#include<atomic>
#include<span>

#include "Units.hpp"

namespace leanstore{
namespace storage{
namespace recordcache{

//=========================================================================================================================================================
//      RecordCache Type State Machine
//=========================================================================================================================================================
enum class RecordCacheType: u8 {
    ReadOnlyMode                                                = 0b000,        // 000: Read-Only mode, Normal Operation.
    WriteThroughMode                                            = 0b001,        // 001: Write-Through mode
    WriteBackMode                                               = 0b010,        // 010: Write-Back mode
    LogicallyDeletedButStillInHashTable                         = 0b011,        // 011: Logical deleted, but still in hash table(wait for Forward_epoch thread)
    RemovedFromHashTableButWaitForPhysicalMemoryDeallocation    = 0b100,       // 100: Removed from hash table, wait for Eviction thread to free memory
    PromoteThreadHoldingThePosition                             = 0b111,       // 111: Promote thread occuping the state
};

//---------------------------------------------------------------------------------------------------------------------------------------------------------------
// RecordCacheEntry: In our design, it contains the four part:
// 
// Logical Layout:
// [ Meta Data: type | key_length | value_length ]
// [ Eviction Control: visited]
// [ Visibility Check: last_modified_worker_id | tx_ts ]
// [ Raw Data: key | value]
//
// Physical Layout(sorted by alignment 8 -> 2 -> 1 to avoid padding)
// [tx_ts(8) | last_modified_worker_id(2) | key_length (2) | value_length (2) | entry_type(1) | visited (1) | payload[]]
// sizeof(RecordCacheEntry) == 16, zero padding waste
struct RecordCacheEntry{
    //--------------------[Added]. Visibility Check -----------------------------
    // Transaction ID/timestamp of the last modification.
    // Highest bit indicates if transaction is committed.
    u64 tx_ts;

    // The MVCC component compatible with legacy logic.
    u16 last_modified_worker_id;

    //--------------------[Added]. Meta Data ------------------------------------
    // Support for variable length keys and values
    u16 key_length;
    u16 value_length;

    std::atomic<RecordCacheType> entry_type;                    // default: 0b000

    //--------------------[Added]. Eviction Control -----------------------------
    // 1-bit atomic visited flag for SIEVE eviction logic.
    std::atomic<bool> visited;

    //--------------------[Added]. Raw Data --------------------------------------
    // Raw Data
    // Flexible array member for Key + Value
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
};
}    
}
}