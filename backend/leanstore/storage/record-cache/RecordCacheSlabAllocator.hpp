#pragma once

#include<memory_resource>
#include<vector>
#include<mutex>
#include<cassert>
#include<stdexcept>
#include<atomic>
#include<memory>
#include<cstdio>
#include<new>
#include<algorithm>
#include<functional>
#include"Units.hpp"
//----------------------------------------------[Added].------------------------------------------------------
// SlabAllocator for RecordCacheEntry
// 1. Take ownership of BufferManager's memory chunk(use mmap to allocate before.)
// 2. 2MB slice and 8 Byte step, start from 24 B ... to 16 KB
// 3. Lazy loading, if some SlabClass, for example 256 B is never used, then do not allocate it.
// 4. Integrate it with c++17 pmr interface.

namespace leanstore{
namespace storage{
namespace recordcache{

// Using C++17 std::pmr
// We override do_allocate and do_deallocate for RecordCacheSlabAllocator
class RecordCacheSlabAllocator: public std::pmr::memory_resource{
public:
    static constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;   // 2 MB, allocating huge page at a time.
    static constexpr size_t BLOCK_STEP = 8;                      // each size class has 8 Byte difference, for example: 32 B, 40B, 48B, 56B ...
    static constexpr size_t MIN_BLOCK_SIZE = 24;                // Minimal size: 16 B (RecordCacheEntry) + 8 Byte Payload
    static constexpr size_t MAX_BLOCK_SIZE = 16 * 1024;         // Maximal size: 16 KB(Up to Page Size)
    static constexpr size_t NUM_CLASSES = (MAX_BLOCK_SIZE - MIN_BLOCK_SIZE) / BLOCK_STEP + 1;

    // OS(mmap)
    // --> record_cache_ptr (a huge size of pre-allocated memory)
    // SlabClass[0] ( 24 Bytes)
    // -> SlabClass[1] ( 32 Bytes)
    // -> ...  
    // -> SlabClass [N] (16 KB)
    // Takes ownership of a pre-allocated memory region starting at record_cache_ptr(allocated via mmap by BufferManager).
    // SlabAllocator manages this region internally:
    // 1. Divides it into 2 MB slabs
    // 2. Each slab is assigned to a SlabClass (24, 32, ... bytes)
    // 3. Maintains per-class free_list to eliminate external fragmentation
    // 4. 8-byte step minimizes internal fragmentation

    RecordCacheSlabAllocator(void *base_ptr, size_t total_size)
    : base_ptr(static_cast<char*>(base_ptr)),
    current_ptr(static_cast<char*>(base_ptr)),
    end_ptr(static_cast<char*>(base_ptr) + total_size),
    total_capacity(total_size),
    max_slab_count(total_size / HUGE_PAGE_SIZE),
    slab_metadata(std::make_unique<SlabMetadata[]>(total_size / HUGE_PAGE_SIZE))
    {
        if(!base_ptr || total_size < HUGE_PAGE_SIZE){
            throw std::invalid_argument("Invalid base pointer: check BufferManager:: record_cache_ptr mmap");
        }

        for(size_t i = 0; i < NUM_CLASSES; i++){
            classes[i].block_size = MIN_BLOCK_SIZE + i * BLOCK_STEP;
            classes[i].free_list = nullptr;             // Lazy loading, only when we really need block_size's chunk, then it is not nullptr
        }
    }

    ~RecordCacheSlabAllocator() override = default;

    // Get current usage ratio
    double getUsageRatio() const {
        if(total_capacity == 0) return 0.0;
        return static_cast<double>(bytes_in_use.load(std::memory_order_relaxed)) /
        static_cast<double>(total_capacity);
    }

    size_t getBytesInUse() const {
        return bytes_in_use.load(std::memory_order_relaxed);
    }

    size_t getTotalCapacity() const {
        return total_capacity;
    }

    size_t getAlignedBlockSize(size_t raw_bytes) const {
        return align_up(raw_bytes);
    }

    // [B-mover v4] Used by SIEVE eviction to bias toward draining the per-class
    // drain target slab. Earlier iterations (absolute / relative thresholds) failed
    // because in steady state every slab in a hot class hovers near the class
    // average — no individual slab sits below threshold. The fix is to pick ONE
    // explicit drain target per class (the slab with the lowest known live_count)
    // and concentrate ALL drain pressure on it: SIEVE force-evicts every entry on
    // it, and do_deallocate sinks every freed block on it to the free_list tail.
    // The target is sticky: once chosen it stays drain target until it reaches
    // live_count==0 and tryReclaimSlab fires; then a new target is chosen by
    // the next do_deallocate in that class.
    //
    // Pointers outside the managed region (e.g. new_delete_resource overflow
    // allocations) return false. Reads relaxed atomic — staleness only weakens the
    // heuristic, never breaks correctness.
    bool isSlabUnderUtilized(const void* p, double /*unused*/ = 0.0) const {
        const char* cp = static_cast<const char*>(p);
        if(cp < base_ptr || cp >= end_ptr) return false;
        size_t idx = static_cast<size_t>(cp - base_ptr) / HUGE_PAGE_SIZE;
        if(idx >= max_slab_count) return false;
        return slab_metadata[idx].is_drain_target.load(std::memory_order_relaxed);
    }

    // [B-mover v5] Synchronous slab rescue plumbing.
    //
    // SIEVE's background drain via force_evict is too slow for burst allocations
    // (per-class drain rate ~ 2 * SIEVE_rate * target.live / class.live; for
    // 408B class with 110 slabs that's ~140 blocks/sec, but workload burst exhausts
    // capacity in seconds). When do_allocate cannot get a slab from the pool nor
    // carve a new one, it calls the rescue callback. The callback is expected to:
    //   (1) pick a slab with low live_count,
    //   (2) unlink every entry on that slab from the hash table (set type=100),
    //   (3) null those entries out of the SIEVE FIFO (to avoid double-queue),
    //   (4) wait for epoch safety,
    //   (5) inline call back into allocator.deallocate so live_count drops to 0,
    //   (6) tryReclaimSlab fires inside the dealloc and pushes idx to free_slab_pool.
    // Callback returns true iff free_slab_pool became non-empty.
    using SlabRescueCallback = std::function<bool()>;
    void setSlabRescueCallback(SlabRescueCallback cb){
        rescue_callback = std::move(cb);
    }

    // [B-mover v5] Cheap probe used by the rescue path to short-circuit when the
    // pool became non-empty between iterations (e.g. another rescuer succeeded
    // while we were waiting on the rescue mutex). Lock-protected so it gives a
    // consistent snapshot but the result is immediately stale on return; the only
    // safe action is "retry the carve loop" — which itself relocks under the
    // class mutex.
    bool hasFreeSlab() const {
        std::lock_guard<std::mutex> lock(free_slab_pool_mutex);
        return !free_slab_pool.empty();
    }

    // [B-mover v5] Rescue picks targets via this method. Returns at most
    // `max_results` currently-assigned slabs sorted ascending by live_count, so
    // the rescuer can try the easiest-to-drain candidates first. block_size==0 is
    // the marker for "this slab is currently in free_slab_pool" — we skip those.
    // Read is lock-free (relaxed atomics on per-slab metadata); the snapshot is
    // approximate but that's fine, rescue is best-effort.
    struct SlabRescueCandidate {
        size_t slab_idx;
        char* slab_base;
        size_t slab_bytes;          // HUGE_PAGE_SIZE — kept for clarity at call sites
        size_t block_size;          // 0 if slab was reclaimed between scan and use
        size_t live_count;          // snapshot; may be slightly stale
    };
    std::vector<SlabRescueCandidate> findRescueCandidates(size_t max_results) const {
        std::vector<SlabRescueCandidate> out;
        out.reserve(max_results * 2);
        for(size_t i = 0; i < max_slab_count; i++){
            size_t bs = slab_metadata[i].block_size.load(std::memory_order_relaxed);
            if(bs == 0) continue;
            size_t lc = slab_metadata[i].live_count.load(std::memory_order_relaxed);
            if(lc == 0) continue;   // already fully drained; reclaim is pending
            out.push_back({
                i,
                base_ptr + i * HUGE_PAGE_SIZE,
                HUGE_PAGE_SIZE,
                bs,
                lc
            });
        }
        std::sort(out.begin(), out.end(), [](const SlabRescueCandidate& a, const SlabRescueCandidate& b){
            return a.live_count < b.live_count;
        });
        if(out.size() > max_results) out.resize(max_results);
        return out;
    }

    // [B-mover v5] Counters surfaced via the bad_alloc diagnostic dump.
    size_t getRescueAttemptedTotal() const { return rescue_attempted_total.load(std::memory_order_relaxed); }
    size_t getRescueSucceededTotal() const { return rescue_succeeded_total.load(std::memory_order_relaxed); }

    // Disable Copy and Move
    RecordCacheSlabAllocator(const RecordCacheSlabAllocator&) = delete;
    RecordCacheSlabAllocator& operator=(const RecordCacheSlabAllocator&) = delete;

protected:
    void* do_allocate(size_t bytes, size_t alignment) override{
        // Check that we have 8B alignment
        assert(alignment <= 8 && "RecordCacheSlabAllocator only supports up to 8-byte alignment");

        // if larger then the largest block size, withdraw back to new_delete_resource
        if(bytes > MAX_BLOCK_SIZE){
            return std::pmr::new_delete_resource() -> allocate(bytes, alignment);
        }

        size_t slab_class_index = GetClassIndex(bytes);
        auto &slab_size_class = classes[slab_class_index];

        // [B-mover v5] unique_lock so we can drop it during a rescue call.
        std::unique_lock<std::mutex> lock(slab_size_class.mutex);

        // Lazy loading, if the size class has no free chunk, then cut 2 MB Huge Page from record_cache_ptr.
        // First attempt: try pool + carve without engaging rescue. Suppress the
        // bad_alloc diagnostic — if we have a rescue callback, the dump should
        // only fire when rescue also fails.
        if(!slab_size_class.free_list){
            if(!allocate_slab_for_class(slab_size_class, /*emit_diag_on_fail=*/false)){
                // Drop class mutex before calling back into RecordCache. Rescue
                // takes shard locks and calls allocator.deallocate (which takes
                // some — possibly this same — class mutex), so holding ours here
                // would risk recursive lock acquisition.
                lock.unlock();

                bool rescued = false;
                if(rescue_callback){
                    rescue_attempted_total.fetch_add(1, std::memory_order_relaxed);
                    rescued = rescue_callback();
                    if(rescued){
                        rescue_succeeded_total.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                lock.lock();
                // Another thread may have freed a block into this class's free_list
                // while we were rescuing; check before reattempting carve.
                if(!slab_size_class.free_list){
                    if(!allocate_slab_for_class(slab_size_class, /*emit_diag_on_fail=*/true)){
                        // Even after rescue we couldn't satisfy the request.
                        // SLAB-SUMMARY has already been dumped to stderr above.
                        throw std::bad_alloc();
                    }
                }
            }
        }

        FreeNode *node = slab_size_class.free_list;
        slab_size_class.free_list = node -> next;
        // [B-mover] Keep tail in sync. If we just emptied the list, tail must
        // also become nullptr so the next push-to-tail can correctly seed it.
        if(slab_size_class.free_list == nullptr){
            slab_size_class.free_list_tail = nullptr;
        }

        bytes_in_use.fetch_add(slab_size_class.block_size, std::memory_order_relaxed);

        // [B-mover] Track per-slab live count so we can detect when a slab is
        // fully drained (and thus reclaimable to the cross-class free pool).
        size_t slab_idx = static_cast<size_t>(reinterpret_cast<char*>(node) - base_ptr) / HUGE_PAGE_SIZE;
        slab_metadata[slab_idx].live_count.fetch_add(1, std::memory_order_acq_rel);

        return static_cast<void*>(node);
    }

    void do_deallocate(void *p, size_t bytes, size_t alignment) override{
        if(!p) return;

        if(bytes > MAX_BLOCK_SIZE){
            std::pmr::new_delete_resource() -> deallocate(p, bytes, alignment);
            return;
        }

        size_t slab_class_index = GetClassIndex(bytes);
        auto &slab_size_class = classes[slab_class_index];

        std::lock_guard<std::mutex> lock(slab_size_class.mutex);

        FreeNode * node = static_cast<FreeNode*>(p);

        // Update global accounting first so getUsageRatio() reflects post-dealloc state.
        bytes_in_use.fetch_sub(slab_size_class.block_size, std::memory_order_relaxed);

        // [B-mover] Decrement the slab's live count. If it hits 0, every block
        // of this slab is now back in the class free_list — reclaim the whole
        // slab and return it to the cross-class free pool. We hold the class
        // mutex throughout the walk, so no concurrent alloc/dealloc can pull
        // a block from this slab mid-reclaim.
        size_t slab_idx = static_cast<size_t>(static_cast<char*>(p) - base_ptr) / HUGE_PAGE_SIZE;
        size_t prev_live = slab_metadata[slab_idx].live_count.fetch_sub(1, std::memory_order_acq_rel);

        // [B-mover v4] Per-class drain target selection. The class holds exactly one
        // drain target slab; ALL drain pressure (SIEVE force-evict + sink-to-tail)
        // converges on it. Earlier threshold-based attempts (absolute / relative)
        // scattered ~100K sinks across many small-class slabs and never accumulated
        // enough pressure on the big 408B / 768B class slabs to drive any of them to
        // live_count==0. Concentrating the pressure on ONE slab per class drains it
        // in ~10 seconds even for a 5141-block 408B slab.
        //
        // Algorithm under class.mutex:
        //   1) If no target yet (SIZE_MAX) → adopt this slab.
        //   2) Else if this slab's live_after < current target's live → switch.
        //   3) sink-to-tail iff this slab IS the current target.
        //   Floor: only run target selection when global utilization ≥ 0.50; below
        //   that there's no slab-shortage pressure and we keep pure LIFO.
        const size_t live_after = (prev_live > 0) ? (prev_live - 1) : 0;
        bool sink_to_tail = false;
        if(live_after > 0){
            const double global_util = getUsageRatio();
            if(global_util >= 0.50){
                size_t cur_target = slab_size_class.drain_target_idx;
                bool become_target = false;
                if(cur_target == SIZE_MAX){
                    become_target = true;
                }else if(cur_target != slab_idx){
                    const size_t target_live = slab_metadata[cur_target].live_count
                        .load(std::memory_order_relaxed);
                    if(live_after < target_live) become_target = true;
                }
                if(become_target){
                    if(cur_target != SIZE_MAX && cur_target != slab_idx){
                        slab_metadata[cur_target].is_drain_target
                            .store(false, std::memory_order_relaxed);
                    }
                    slab_size_class.drain_target_idx = slab_idx;
                    slab_metadata[slab_idx].is_drain_target
                        .store(true, std::memory_order_relaxed);
                    drain_target_switch_total.fetch_add(1, std::memory_order_relaxed);
                }
                if(slab_size_class.drain_target_idx == slab_idx){
                    sink_to_tail = true;
                }
            }
        }

        if(sink_to_tail){
            node->next = nullptr;
            if(slab_size_class.free_list_tail == nullptr){
                // List was empty: this node is both head and tail.
                slab_size_class.free_list = node;
                slab_size_class.free_list_tail = node;
            }else{
                slab_size_class.free_list_tail->next = node;
                slab_size_class.free_list_tail = node;
            }
            sunk_to_tail_total.fetch_add(1, std::memory_order_relaxed);
        }else{
            // Normal LIFO push to head.
            node->next = slab_size_class.free_list;
            slab_size_class.free_list = node;
            if(slab_size_class.free_list_tail == nullptr){
                slab_size_class.free_list_tail = node;  // first item, also tail
            }
        }

        if(prev_live == 1){
            tryReclaimSlab(slab_idx, slab_size_class);
        }
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    struct FreeNode{
        FreeNode *next;
    };

    struct SlabSizeClass{
        std::mutex mutex;           // Every SlabSizeClass's concurrency safety.
        FreeNode* free_list{nullptr};
        // [B-mover] Tail pointer for asymmetric FIFO/LIFO push in do_deallocate.
        // Blocks freed on the drain target sink to the tail; blocks on other slabs
        // go to the head (LIFO). This way the head stays warm with cyclable blocks
        // while target-slab blocks pile up at the tail and don't get reused — the
        // target slab keeps losing live_count until tryReclaimSlab fires.
        FreeNode* free_list_tail{nullptr};
        size_t block_size;
        std::atomic<size_t> slab_count{0};  // [N4-DIAG] how many 2 MB slabs this class has consumed
        // [B-mover v4] The slab_idx that all drain pressure (SIEVE force-evict +
        // sink-to-tail) for this class is concentrated on. SIZE_MAX = no target.
        // Read/written only while holding `mutex`, so no atomic required.
        size_t drain_target_idx{SIZE_MAX};
    };

    // [B-mover] Per-slab metadata indexed by slab_idx = (ptr - base_ptr) / HUGE_PAGE_SIZE.
    // total_blocks==0 means the slab is currently sitting in free_slab_pool, unassigned to
    // any class. live_count is atomically bumped/decremented on every do_allocate/do_deallocate.
    // When live_count drops to 0 under the class mutex, tryReclaimSlab returns the whole
    // slab to free_slab_pool so a different size class can reuse it later.
    struct SlabMetadata {
        std::atomic<size_t> live_count{0};
        std::atomic<size_t> block_size{0};
        std::atomic<size_t> total_blocks{0};
        // [B-mover v4] Set by do_deallocate when this slab becomes its class's
        // drain target; cleared on target switch or tryReclaimSlab. SIEVE reads
        // this via isSlabUnderUtilized to force-evict every entry on this slab.
        std::atomic<bool> is_drain_target{false};
    };

    SlabSizeClass classes[NUM_CLASSES];

    // Global Memory Management
    // Start from base_ptr, every time we have *current_ptr + 2 MB
    char *base_ptr;
    char *current_ptr;
    char *end_ptr;
    size_t total_capacity;
    std::mutex already_allocated_large_chunk_memory;        // Protect that we cut 2 MB slab from record_cache_ptr
    std::atomic<size_t> bytes_in_use{0};

    // [B-mover] Per-slab tracking + cross-class free pool.
    // slab_metadata is a fixed-size array sized at construction (max_slab_count =
    // total_capacity / HUGE_PAGE_SIZE). free_slab_pool holds slab_idx of fully drained
    // slabs that can be re-carved for any size class. Protected by free_slab_pool_mutex.
    size_t max_slab_count;
    std::unique_ptr<SlabMetadata[]> slab_metadata;
    // mutable: hasFreeSlab() is a const observer used by the rescue caller; the
    // mutex itself does not change observable state, only synchronizes a peek.
    mutable std::mutex free_slab_pool_mutex;
    std::vector<size_t> free_slab_pool;
    std::atomic<size_t> reclaimed_slab_total{0};            // [N4-DIAG] cumulative reclaim events
    std::atomic<size_t> sunk_to_tail_total{0};              // [N4-DIAG] cumulative sink-to-tail dealloc events
    std::atomic<size_t> drain_target_switch_total{0};       // [N4-DIAG] cumulative drain-target changes
    std::atomic<size_t> rescue_attempted_total{0};          // [N4-DIAG] bad_alloc rescue invocations
    std::atomic<size_t> rescue_succeeded_total{0};          // [N4-DIAG] rescues that produced a reclaimed slab

    // [B-mover v5] Set by RecordCache::startBackgroundThreads via setSlabRescueCallback.
    // Called from do_allocate when allocate_slab_for_class first fails — see top of class
    // for the contract.
    SlabRescueCallback rescue_callback;

    size_t align_up(size_t size) const{
        if(size <= MIN_BLOCK_SIZE) return MIN_BLOCK_SIZE;
        return (size + BLOCK_STEP - 1) & ~(BLOCK_STEP - 1);
    }

    size_t GetClassIndex(size_t size) const {
        size_t aligned = align_up(size);
        assert(aligned <= MAX_BLOCK_SIZE);

        return (aligned - MIN_BLOCK_SIZE) / BLOCK_STEP;
    }

    // If we require a slab_size_class.slab_size 's chunk
    // Then we (1) require 2 MB from record_cache_ptr (2) cut the 2 MB to form a slab_sized free linked-list.
    //
    // [B-mover v5] Returns true on success, false on exhaustion. Caller in
    // do_allocate is responsible for either invoking the rescue callback and
    // retrying, or throwing bad_alloc. `emit_diag_on_fail=false` suppresses the
    // SLAB-SUMMARY dump on the first failure (so a successful rescue stays
    // silent); the second/final attempt should pass `true`.
    bool allocate_slab_for_class(SlabSizeClass &slab_size_class, bool emit_diag_on_fail = true){
        char *slab = nullptr;
        size_t slab_idx = 0;
        bool reused_from_pool = false;

        // [B-mover] Step 0: Try to reuse a fully-drained slab from the cross-class
        // free pool before carving a fresh one from the mmap region. This is what
        // lets a 408B class release a slab and have a 56B class pick it up later.
        {
            std::lock_guard<std::mutex> pool_lock(free_slab_pool_mutex);
            if(!free_slab_pool.empty()){
                slab_idx = free_slab_pool.back();
                free_slab_pool.pop_back();
                slab = base_ptr + slab_idx * HUGE_PAGE_SIZE;
                reused_from_pool = true;
            }
        }

        // Step 1: Cut 2 MB from already allocated large chunk of memory
        if(slab == nullptr){
            std::lock_guard<std::mutex> lock(already_allocated_large_chunk_memory);
            if(current_ptr + HUGE_PAGE_SIZE > end_ptr){
                if(emit_diag_on_fail){
                    // [N4-DIAG] Distinguish two failure modes:
                    //   (A) physical exhaustion: most of total_capacity already carved into
                    //       per-class slabs (current_ptr near end_ptr) — even if many blocks
                    //       are free, SIEVE cannot recycle slabs back to the global pool.
                    //   (B) logical exhaustion: bytes_in_use ~ total_capacity — SIEVE was
                    //       supposed to evict but didn't keep up.
                    const size_t carved_bytes = static_cast<size_t>(current_ptr - base_ptr);
                    const size_t inuse = bytes_in_use.load(std::memory_order_relaxed);
                    fprintf(stderr,
                        "[N4-DIAG][SLAB] bad_alloc raised: total_capacity=%zu bytes  "
                        "physical_carved=%zu (%.1f%%)  bytes_in_use=%zu (%.1f%%)  "
                        "requested_slab=%zu  requesting_block_size=%zu\n",
                        total_capacity,
                        carved_bytes, 100.0 * carved_bytes / static_cast<double>(total_capacity),
                        inuse,        100.0 * inuse        / static_cast<double>(total_capacity),
                        HUGE_PAGE_SIZE, slab_size_class.block_size);
                    // [N4-DIAG] Dump every class that has ever pulled a slab — this tells us
                    // how many distinct size classes TPC-C actually exercises, and where the
                    // 2 MB residual waste is concentrated. Read slab_count via relaxed atomic;
                    // do NOT take per-class mutex (would invert lock order and risk deadlock).
                    size_t used_class_count = 0;
                    size_t total_slabs_accounted = 0;
                    for(size_t i = 0; i < NUM_CLASSES; i++){
                        size_t sc = classes[i].slab_count.load(std::memory_order_relaxed);
                        if(sc > 0){
                            used_class_count++;
                            total_slabs_accounted += sc;
                            fprintf(stderr,
                                "[N4-DIAG][SLAB-CLASS] class_index=%zu block_size=%zu slabs=%zu (%.2f MB)\n",
                                i, classes[i].block_size, sc,
                                static_cast<double>(sc * HUGE_PAGE_SIZE) / (1024.0 * 1024.0));
                        }
                    }
                    fprintf(stderr,
                        "[N4-DIAG][SLAB-SUMMARY] used_classes=%zu total_slabs=%zu (%.2f MB)  reclaim_events=%zu  sunk_to_tail=%zu  drain_target_switches=%zu  rescue_attempted=%zu  rescue_succeeded=%zu\n",
                        used_class_count, total_slabs_accounted,
                        static_cast<double>(total_slabs_accounted * HUGE_PAGE_SIZE) / (1024.0 * 1024.0),
                        reclaimed_slab_total.load(std::memory_order_relaxed),
                        sunk_to_tail_total.load(std::memory_order_relaxed),
                        drain_target_switch_total.load(std::memory_order_relaxed),
                        rescue_attempted_total.load(std::memory_order_relaxed),
                        rescue_succeeded_total.load(std::memory_order_relaxed));
                    fflush(stderr);
                }
                return false;   // Let do_allocate decide whether to rescue or throw.
            }
            slab = current_ptr;
            slab_idx = static_cast<size_t>(current_ptr - base_ptr) / HUGE_PAGE_SIZE;
            current_ptr += HUGE_PAGE_SIZE;
        }
        // [N4-DIAG] slab_count is still tracked (it feeds SLAB-CLASS dump on bad_alloc),
        // but we no longer fprintf per carve — that was ~hundreds of stderr syscalls
        // per second on the allocator hot path. (void)reused_from_pool to keep the
        // local meaningful for readers.
        slab_size_class.slab_count.fetch_add(1, std::memory_order_relaxed);
        (void)reused_from_pool;

        size_t block_size = slab_size_class.block_size;
        size_t num_of_that_blocks = HUGE_PAGE_SIZE / block_size;

        // [B-mover] Initialize this slab's metadata. live_count starts at 0; it will be
        // bumped by the do_allocate that immediately follows. block_size != 0 marks the
        // slab as "owned by a size class" so isSlabUnderUtilized and tryReclaimSlab
        // can recognize it.
        slab_metadata[slab_idx].block_size.store(block_size, std::memory_order_release);
        slab_metadata[slab_idx].total_blocks.store(num_of_that_blocks, std::memory_order_release);
        slab_metadata[slab_idx].live_count.store(0, std::memory_order_release);

        // Step 2: Cut 2 MB's Slab into block_size's chunk, and chain it to a linkedlist
        for(size_t i = 0; i < num_of_that_blocks - 1; i++){
            FreeNode* node = reinterpret_cast<FreeNode*>(slab + i * block_size);
            node -> next = reinterpret_cast<FreeNode*>(slab + (i + 1) * block_size);
        }
        FreeNode* last_node = reinterpret_cast<FreeNode*>(slab + (num_of_that_blocks - 1) * block_size);
        last_node -> next = nullptr;

        slab_size_class.free_list = reinterpret_cast<FreeNode*>(slab);
        // [B-mover] Seed tail so the first sink-to-tail push appends correctly.
        slab_size_class.free_list_tail = last_node;
        return true;
    }

    // [B-mover] Called from do_deallocate when a slab's live_count drops to 0.
    // Caller must hold slab_size_class.mutex. We walk the class's free_list, unlink every
    // FreeNode whose address falls inside [slab_base, slab_base+HUGE_PAGE_SIZE), then push
    // the slab_idx onto free_slab_pool so a different size class can claim it next time.
    //
    // Why this is safe to call without any RecordCacheEntry-level coordination:
    //   - live_count == 0 means no live entry exists on this slab. By definition every
    //     block of this slab must currently sit in the class free_list (no other place
    //     for them to be), so the walk will find num_of_that_blocks FreeNodes to remove.
    //   - We hold the class mutex throughout, so no racing do_allocate can pop a freed
    //     block from this slab between our checks.
    //   - The hash table, SIEVE FIFO, InvalidationQueue, and PromoteThread Phase 2 never
    //     hold pointers into blocks counted by live_count once the corresponding
    //     deallocate has completed — that's the contract of ForwardEpoch.
    void tryReclaimSlab(size_t slab_idx, SlabSizeClass &slab_size_class){
        char* slab_begin = base_ptr + slab_idx * HUGE_PAGE_SIZE;
        char* slab_end   = slab_begin + HUGE_PAGE_SIZE;

        // Walk free_list once, partition into "keep" (not on this slab) vs "drop".
        FreeNode* new_head = nullptr;
        FreeNode* new_tail = nullptr;
        size_t removed = 0;
        size_t kept = 0;

        FreeNode* cur = slab_size_class.free_list;
        while(cur != nullptr){
            FreeNode* next = cur->next;
            char* cp = reinterpret_cast<char*>(cur);
            if(cp >= slab_begin && cp < slab_end){
                removed++;
            }else{
                if(new_head == nullptr){
                    new_head = cur;
                }else{
                    new_tail->next = cur;
                }
                new_tail = cur;
                kept++;
            }
            cur = next;
        }
        if(new_tail) new_tail->next = nullptr;
        slab_size_class.free_list = new_head;
        // [B-mover] Keep tail pointer consistent after partitioning.
        slab_size_class.free_list_tail = new_tail;  // nullptr if class list is now empty

        // Mark slab as unassigned (block_size=0 / total_blocks=0). live_count is already 0.
        const size_t old_block_size = slab_metadata[slab_idx].block_size.load(std::memory_order_relaxed);
        slab_metadata[slab_idx].block_size.store(0, std::memory_order_release);
        slab_metadata[slab_idx].total_blocks.store(0, std::memory_order_release);
        // [B-mover v4] Clear drain target state. If this slab was the class's
        // target (which it almost always is — that's how it got drained), reset
        // the class pointer so the next do_deallocate picks a new target.
        slab_metadata[slab_idx].is_drain_target.store(false, std::memory_order_relaxed);
        if(slab_size_class.drain_target_idx == slab_idx){
            slab_size_class.drain_target_idx = SIZE_MAX;
        }

        // The class consumed one less slab.
        slab_size_class.slab_count.fetch_sub(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> pool_lock(free_slab_pool_mutex);
            free_slab_pool.push_back(slab_idx);
        }
        // [N4-DIAG] reclaimed_slab_total still tracked (SLAB-SUMMARY references it on
        // bad_alloc dump). No per-event fprintf — was the dominant stderr source.
        reclaimed_slab_total.fetch_add(1, std::memory_order_relaxed);
        (void)old_block_size; (void)removed; (void)kept;
    }
};

}
}
}

//
// Usage example:
// (1) Wrapper part:
// Using pmr::polymorphic_allocator to wrap our memory_resource
//
// leanstore::storage::recordcache::RecordCacheSlabAllocator slab_allocator(buffer_manager.record_cache_ptr, FLAGS_dram_record_cache_gib);
// std::pmr::polymorphic_allocator<std::byte> alloc(&slab_allocator);
// 
// (2) Usage part: If a RecordCacheEntry has 256 B.
// RecordCacheEntry * record_1 = alloc.allocate_object<RecordCacheEntry>();
// 
// [Inner function]:
// do_allocate(256)
// --> align_up(256)
// --> get_class_index(256) = 29
// --> classes[29] -> (first usage): lazy loading
//          --------> allocate_slab_for_class(sc)
//          ---------> 1. Cut 2 MB from record_cache_ptr 2. divide into 8192 blocks 3. linked into free_list
//          ---------> Pop a block from free_list