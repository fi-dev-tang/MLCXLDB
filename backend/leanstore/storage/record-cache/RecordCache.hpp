#pragma once

#include<shared_mutex>
#include<unordered_map>
#include<string_view>
#include<memory>
#include<thread>
#include<queue>
#include<condition_variable>
#include<span>
#include<atomic>
#include<cstring>

#include"RecordCacheEntry.hpp"
#include"RecordCacheSlabAllocator.hpp"
#include"SIEVE_eviction/SieveFIFO.hpp"
#include"../../Config.hpp"

#include"Forward_epoch/EpochManager.hpp"
#include"Forward_epoch/InvalidationQueue.hpp"

namespace leanstore{
namespace storage{
    // BufferFrame's full definition arrives transitively via RecordCacheEntry.hpp
    // (RecordCacheEntry now stores a Swip<BufferFrame>, whose header pulls in BufferFrame.hpp).
}
namespace btree{
    class BTreeNode;
    class BTreeVI;
}
}

namespace leanstore{
namespace storage{
namespace recordcache{

//=================================[Added].=========================================================
// RecordCache(HashTable)
// Hash(key) -> RecordCacheEntry*
// Using Chained hash table here, 
// we implement it using multiple shards combinded with std::unordered_map

// std::unordered_map hash function implementation
// In order to guarantee high-performance concurrent hash maps with frequent updates,
// we use std::string as the key to guarantees memory safety, 
// even if we have invalidation algorithm based on RCU(Read-Copy-Update),
// and manage memory reclaimation ourselves.


// Hash Function: FNV-1a
struct FNV1aHash{
    using is_transparent = void;            // Adding is_transparent to support heterogeneous lookup.
    // Support operator() for std::string_view
    size_t operator()(std::string_view key) const noexcept{
        u64 hash = 14695981039346656037ULL;
        for(unsigned char c: key){
            hash ^= static_cast<u64>(c);
            hash *= 1099511628211ULL;
        }
        return static_cast<size_t>(hash);
    }

    // Support operator() for std::string
    size_t operator()(const std::string& key) const noexcept{
        return (*this)(std::string_view(key));
    }
};

// ConcurrentHashShard (we have multiple shards + std::unordered_map)
struct alignas(64) ConcurrentHashShard{
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, RecordCacheEntry*, FNV1aHash, std::equal_to<void>> hash_map_shard;
};

// Manage multiple ConcurrentHashShards
// locate key to corresponding ConcurrentHashShard
class RecordCache{
private:
    std::unique_ptr<ConcurrentHashShard[]> hash_shards;
    size_t num_of_shards;
    RecordCacheSlabAllocator& allocator;
    mutable size_t record_cache_total_size = 0;

    std::atomic<size_t> active_entry_count{0};
    size_t logical_capacity{0};

    static inline u64 HashBytes(std::span<const u8> key){
        u64 hash = 14695981039346656037ULL;
        for(u8 b: key){
            hash ^= static_cast<u64>(b);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static inline std::string MakeKeyOwningInStringFormat(std::span<const u8> key){
        return std::string(reinterpret_cast<const char*>(key.data()), key.size());
    }

    // [v4-a] Build a (dt_id || key) lookup key into a caller-provided stack buffer.
    // When FLAGS_rc_skip_dt_id_prefix=true (single-table workloads only — YCSB),
    // the 2B dt_id is omitted and the returned span aliases the original key
    // (zero memcpy on the hot path). Caller must size buf >= 2 + key.size().
    // TPC-C MUST keep this flag false because customer/order/neworder all use the
    // same Integer w,d,id → 12B folded key shape and would collide in the byte map.
    static inline std::span<const u8> BuildPrefixedKey(u16 dt_id,
                                                       std::span<const u8> key,
                                                       u8* buf) {
        if (FLAGS_rc_skip_dt_id_prefix) {
            return key;
        }
        std::memcpy(buf, &dt_id, 2);
        std::memcpy(buf + 2, key.data(), key.size());
        return std::span<const u8>(buf, 2 + key.size());
    }

    // Same as BuildPrefixedKey but allocates an owning std::string — used by
    // signalPromoteThread because PromoteRequestMessage outlives the caller frame
    // (the original key bytes may be on a worker stack about to unwind).
    static inline std::string BuildPrefixedKeyOwning(u16 dt_id,
                                                     std::span<const u8> key) {
        std::string out;
        if (FLAGS_rc_skip_dt_id_prefix) {
            out.assign(reinterpret_cast<const char*>(key.data()), key.size());
        } else {
            out.reserve(2 + key.size());
            out.append(reinterpret_cast<const char*>(&dt_id), 2);
            out.append(reinterpret_cast<const char*>(key.data()), key.size());
        }
        return out;
    }

    // GetShardIndex from Hash,
    // we only calculate hash value once, instead of calling GetShardIndexFromKey,
    // GetShardIndexFromKey requires two times of hash value calculation.
    inline size_t GetShardIndexFromHash(u64 hash) const {
        return static_cast<size_t> (hash % num_of_shards);
    }

    inline ConcurrentHashShard& getShardByHash(u64 hash){
        return hash_shards[GetShardIndexFromHash(hash)];
    }

    inline const ConcurrentHashShard& getShardByHash(u64 hash) const {
        return hash_shards[GetShardIndexFromHash(hash)];
    }

//===================================[Added].========================================================
//                      BackgroundThread Management.
//====================================================================================================
private:
    EpochManager epoch_manager;
    InvalidationQueue invalidation_queue;

    std::atomic<u64> bg_threads_counter = 0;
    std::atomic<bool> bg_threads_keep_running = true;
    // Number of entries physically reclaimed by SIEVE eviction thread.
    std::atomic<u64> sieve_eviction_entries{0};
    // [FIX-C] Pending dead entries: type=100 residue still sitting in the SIEVE
    // FIFO, holding slab blocks even though they have already been unlinked from
    // the hash table and queued to InvalidationQueue. Incremented by worker /
    // PromoteThread when they transition an entry to 100, decremented by SIEVE
    // when it physically drains the entry pointer out of the FIFO. When this
    // counter crosses kPendingDeadEntryThreshold, SIEVE wakes proactively
    // (dead_pile path) without waiting for the 0.90 slab watermark — this is
    // the "invalidation triggers fast eviction" signal.
    std::atomic<u64> pending_state_invalidated_from_hash{0};
    std::vector<std::thread> record_cache_background_threads;

//===================================[Added].========================================================
//                      BackgroundThread Management.
//====================================================================================================
//===================================[Added].========================================================
//                      PromoteThread requested communication queue.
//====================================================================================================
public:
    struct PromoteRequestMessage{
        std::string key;
        BufferFrame* bf;
        PID pid;
        u16 slot_id;
        u16 key_length;
        u16 value_length;
    };

private:
    std::queue<PromoteRequestMessage> promote_request_message_queue;
    std::mutex promote_request_queue_mutex;
    std::condition_variable promote_request_cv;

//===================================[Added].========================================================
//                      PromoteThread requested communication queue.
//====================================================================================================

public:
    explicit RecordCache(
        RecordCacheSlabAllocator& allocator,
        size_t num_of_shards = FLAGS_worker_threads
    ): num_of_shards(num_of_shards == 0 ? 1: num_of_shards), allocator(allocator){
        hash_shards = std::make_unique<ConcurrentHashShard[]>(this -> num_of_shards);
    }

    ~RecordCache() {
        stopBackgroundThreads();
    }

    // Disable Move and Copy(constructor)
    RecordCache(const RecordCache&) = delete;
    RecordCache& operator=(const RecordCache&) = delete;
    RecordCache(RecordCache &&) = delete;
    RecordCache& operator=(RecordCache&&) = delete;

    size_t shardCount() const noexcept {return num_of_shards;}


    //==========================================================================================
    // The following are three important helper function:
    // we use std::unordered_map 's find, insert_or_assign, erase 
    // to represent three function:
    // 1. GetFromRecordCache
    // 2. InsertOrAssignInRecordCache
    // 3. EraseFromRecordCache
    //==========================================================================================
    // HashTable Get function
    // Read: shared_lock
    // [v4-a] Heterogeneous lookup: pass std::string_view directly to find().
    // FNV1aHash has is_transparent and std::equal_to<void> is transparent, so
    // C++20's unordered_map::find supports string_view without constructing a
    // temporary std::string per lookup.
    RecordCacheEntry* GetFromRecordCache(std::span<const u8> key) const {
        const u64 hash_value = HashBytes(key);
        const auto& target_shard = getShardByHash(hash_value);

        std::shared_lock lock(target_shard.mutex);
        std::string_view key_sv(reinterpret_cast<const char*>(key.data()), key.size());
        auto it = target_shard.hash_map_shard.find(key_sv);
        if(it == target_shard.hash_map_shard.end()) return nullptr;
        return it -> second;
    }

    // Return true iff hash table was modified(insert or assign).
    // RecordCache bottom interface(do not handle update / promote Type state conflicts).
   bool InsertOrAssignInRecordCache(std::span<const u8> key, RecordCacheEntry* entry){
        // Semantics:
        // key not exists: insert allowed
        // key exists: do nothing, return operation failed.
        const u64 hash_value = HashBytes(key);
        auto& target_guard = getShardByHash(hash_value);

        bool op_succeeded = false;      // true if we actually changed hash_map_shard
        {
            std::unique_lock lock(target_guard.mutex);
            auto key_str = MakeKeyOwningInStringFormat(key);

            auto existing_item = target_guard.hash_map_shard.find(key_str);

            if(existing_item == target_guard.hash_map_shard.end()){
                // key not exist -> insert.
                target_guard.hash_map_shard.emplace(key_str, entry);
                op_succeeded = true;

            }else{
                // key exists
                op_succeeded = false;
            }
        }

        if(op_succeeded){
            active_entry_count.fetch_add(1, std::memory_order_relaxed);
            sieve_fifo_queue.InsertIntoSieveFIFO(entry);
        }
        return op_succeeded;
    }


    // HashTable Erase function
    // Write: Exclusive_lock
    // [v4-a] Use transparent string_view find() + iterator-version erase() to avoid
    // constructing a per-call std::string. unordered_map::erase(key) is not
    // heterogeneous, but erase(iterator) is — and find() is transparent thanks to
    // FNV1aHash::is_transparent + std::equal_to<void>.
    bool EraseFromRecordCache(std::span<const u8> key){
        const u64 hash_value = HashBytes(key);
        auto& target_shard = getShardByHash(hash_value);

        std::unique_lock lock(target_shard.mutex);

        std::string_view key_sv(reinterpret_cast<const char*>(key.data()), key.size());
        auto it = target_shard.hash_map_shard.find(key_sv);
        if(it == target_shard.hash_map_shard.end()){
            return false;
        }

        target_shard.hash_map_shard.erase(it);
        active_entry_count.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    void SetRecordCacheSize(){
        record_cache_total_size = 0;
        for(size_t i = 0; i < num_of_shards; i++){
            const auto& shard = hash_shards[i];
            std::shared_lock lock(shard.mutex);
            record_cache_total_size += shard.hash_map_shard.size();
        }
    }

    // Avoid Calling this function 
    inline ConcurrentHashShard& getShard(std::span<const u8> key){
        return getShardByHash(HashBytes(key));
    }



//===================================[Added].=========================================================
//                      BackgroundThread Management.
//====================================================================================================
public:
    void startBackgroundThreads();
    void stopBackgroundThreads();
    void forwardEpochThread();
    u64 getSieveEvictionEntries() const { return sieve_eviction_entries.load(std::memory_order_relaxed); }

    // WaterMark for sieveEviction
    static constexpr double EvictionWaterMark = 0.90;

    void sieveEvictionThread();

    // PromoteThread Related
    void promoteThread();
    void processOnPromotionRequest(const PromoteRequestMessage& request);

public:
    SieveFIFOQueue sieve_fifo_queue;

//===================================[Added].=========================================================
//                      Epoch and Invalidation Interface for Worker Threads
//====================================================================================================
public:
    // Worker thread calls these when entering/leaving RecordCache operations
    void enterEpoch(u64 worker_id) {
        epoch_manager.worker_thread_enter_epoch(worker_id);
    }

    void leaveEpoch(u64 worker_id) {
        epoch_manager.worker_thread_leave_epoch(worker_id);
    }

    // Get current global epoch
    u64 getCurrentEpoch() const {
        return epoch_manager.get_global_epoch();
    }

    // Worker thread calls this when logically deleting a RecordCacheEntry
    void addToInvalidationQueue(RecordCacheEntry* entry, u64 update_epoch) {
        invalidation_queue.push(entry, update_epoch);
    }

    // [FIX-C] pending_state_invalidated_from_hash accessors. See the field
    // declaration above for ownership semantics.
    u64 GetPendingStateInvalidatedFromHash() const {
        return pending_state_invalidated_from_hash.load(std::memory_order_relaxed);
    }
    void IncrementPendingStateInvalidatedFromHash() {
        pending_state_invalidated_from_hash.fetch_add(1, std::memory_order_relaxed);
    }
    void DecrementPendingStateInvalidatedFromHash() {
        pending_state_invalidated_from_hash.fetch_sub(1, std::memory_order_relaxed);
    }

    // Debug helpers for tests
    size_t debugInvalidationQueueSize() const {
        return invalidation_queue.approximate_size();
    }

    size_t debugHashTableEntries() const {
        size_t total = 0;
        for (size_t i = 0; i < num_of_shards; i++) {
            const auto& shard = hash_shards[i];
            std::shared_lock lock(shard.mutex);
            total += shard.hash_map_shard.size();
        }
        return total;
    }

    double GetRecordCacheFillRatio() const {
        if (logical_capacity == 0) return allocator.getUsageRatio();
        return static_cast<double>(active_entry_count.load(std::memory_order_relaxed))
             / static_cast<double>(logical_capacity);
    }

    void setLogicalCapacity(size_t cap) { logical_capacity = cap; }
    void setLogicalCapacityFromEntrySize(size_t avg_entry_bytes) {
        if (avg_entry_bytes > 0) {
            size_t aligned = allocator.getAlignedBlockSize(avg_entry_bytes);
            logical_capacity = allocator.getTotalCapacity() / aligned;
        }
    }
    size_t getLogicalCapacity() const { return logical_capacity; }
    size_t getActiveEntryCount() const { return active_entry_count.load(std::memory_order_relaxed); }

//===================================[Added].========================================================
//                      Epoch and Invalidation Interface
//====================================================================================================

//===================================[Added].========================================================
//                      PromoteThread requested Wakeup.
//====================================================================================================
public:
    // Called by Worker Thread(Lookup path) to submit a PromoteRequestMessage.
    // PromoteThread will asynchronously read value from BufferFrame and insert into RecordCache.
    //
    // dt_id is prepended (2 bytes, LE) to the stored key so two tables that share a byte-identical
    // folded-key layout (e.g. TPC-C customer/order/neworder all use Integer w,d,id → 12B) cannot
    // collide in this byte-keyed hash table. When FLAGS_rc_skip_dt_id_prefix=true (single-table
    // workloads only — YCSB), the prefix is omitted; see BuildPrefixedKeyOwning.
    void signalPromoteThread(u16 dt_id, std::span<const u8> key, BufferFrame *bf, PID pid, u16 slot_id, u16 value_length){
        std::string prefixed_key = BuildPrefixedKeyOwning(dt_id, key);
        const u16 stored_key_length = static_cast<u16>(prefixed_key.size());
        std::lock_guard<std::mutex> lock(promote_request_queue_mutex);
        promote_request_message_queue.push({
            std::move(prefixed_key),
            bf,
            pid,
            slot_id,
            stored_key_length,
            value_length
        });
        promote_request_cv.notify_one();
    }
//===================================[Added].========================================================
//                      PromoteThread requested Wakeup.
//====================================================================================================

//===================================[Added].==========================================================
//                      Lookup Interceptor: tryLookupInRecordCache
//======================================================================================================
public:
    // dt_id is prepended (2 bytes, LE) to the looked-up byte key. See signalPromoteThread for why.
    bool tryLookupInRecordCache(u16 dt_id,
                                std::span<const u8> key,
                                const std::function<void(const u8*, u16)>& payload_callback,
                                u64 worker_id);

    // Write-Through sync interceptor:
    // Called AFTER the CXL Page has been written by the BTree update callback.
    // Synchronously copies the new value (and refreshed Swip / slot_id / MVCC fields)
    // into the resident RecordCacheEntry under SeqLock, so subsequent lookups see
    // the freshest version without falling back to CXL.
    //
    // Returns true if the cache entry was synchronized, false if the entry was
    // not present, was in a transitional state (111), already unlinked (100),
    // or the value length did not match (variable-length update).
    bool tryWriteThroughSync(u16 dt_id,
                             std::span<const u8> key,
                             std::span<const u8> new_value,
                             BufferFrame* bf,
                             u16 slot_id,
                             u16 worker_id,
                             u64 tx_ts);

    // Remove interceptor:
    // The record is being deleted on CXL — drop the cache entry as well. Unlinks
    // from the hash table and queues the slab chunk for ForwardEpoch's
    // epoch-safe deallocation.
    bool tryInvalidateForRemove(u16 dt_id, std::span<const u8> key, u64 worker_id);

    // [B-mover v5] Synchronous slab rescue path. Wired into the allocator via
    // setSlabRescueCallback at startBackgroundThreads time. Called when the
    // allocator can neither pop from the cross-class pool nor carve a fresh
    // slab. Picks the lowest-live slab (currently assigned to some size class),
    // unlinks every type=001 entry on that slab from the hash table, nullifies
    // those entries in the SIEVE FIFO (to prevent SIEVE from re-queueing them
    // for invalidation and causing a double-free), waits for epoch safety, then
    // synchronously calls allocator.deallocate on each unlinked entry. The last
    // deallocate drives live_count to 0 and triggers tryReclaimSlab inside the
    // allocator, pushing the slab_idx onto free_slab_pool.
    //
    // Returns true iff free_slab_pool became non-empty (allocator may retry).
    bool tryRescueSlabForAllocator();

private:
    // Serializes concurrent bad_alloc rescue attempts. Only one rescuer at a
    // time iterates the hashtable + advances the epoch. Other concurrent
    // rescuers wait, then short-circuit if the first one already produced a
    // free slab.
    std::mutex slab_rescue_mutex;
};
}
}
}