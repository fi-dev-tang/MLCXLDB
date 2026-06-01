#pragma once
#include<atomic>
#include<array>
#include<bitset>
#include<cmath>
#include<unordered_map>
#include<unordered_set>
#include<vector>
#include<iostream>
#include<sstream>
#include<iomanip>
#include<string>
#include"Units.hpp"
#include"CountMinSketch.hpp"
#include "leanstore/Config.hpp"
#include"leanstore/storage/buffer-manager/BufferManager.hpp"
#include<shared_mutex>

namespace leanstore::storage {
struct BufferFrame;
class BufferManager;
}

//--------------------------------[Added].---------------------------------------------
// This is the second level of our admission-control
// 
// After Calling SampledVisitHistogram's OnPageAccess, we get the coarse and fine threshold
// The we push the page_id into dram_hot_page_condidates
// Then we start RecordCountMinSketch, using (page_id, slot_id) as key
// If the hot page is uniformly visited, we promote it to the DRAM Buffer Pool
// If the hot page is skewly visited, we promote it to the DRAM RecordCache
//
// dram_hot_page_candidates:
// < page_id, std::tuple<total_page_visit_count, page_slot_num, uniform_or_skew, std::bitset[distinct_record_slot_set]<page_slot_num>, std::bitset[already_promoted_slot_set]<page_slot_num>>
// (1) distinct_accessed_slot_set: if exceeds 50% of the page_slot_num, without trigger skew, then uniform
// (2) already_promoted_slot_set: the record that has been admitted into RecordCache
//
// Future Optimization:
// Can use std::atomic<uin64_t> [3] instead of heavy-mutex on std::bitset.
namespace leanstore::storage::two_level_admission_control{

//----------------------------------------------------------------------------------
//  dram_hot_page_candidates
//----------------------------------------------------------------------------------
//
// In LeanStore Layout, 
// Every page has a 32 B Page Header
// Every record has a MVCC-Tuple header and In-Place-Update Chain, using 21 B
// Every record has key_length, value_length, offset and public prefix: using 10 B
// Our payload is at least 8 B
// BTree has 50% fill factor.
// We test that 8 B payload in stable state, has 175 Slots.
// Use 192 for 64-bit align, may future improve it to 256 if necessary.
// std::bitset<MAX_SLOTS_PER_PAGE> 
// std::atomic<u64> [3]
constexpr u16 MAX_SLOTS_PER_PAGE = 192;

struct HotPageCandidate{
    u64 page_cms_visit_count;                  // visit_count snapshot from first-level PageCountMinSketch
    u16 page_slot_num;                      // Number of slots on this page
    leanstore::storage::BufferFrame* cxl_bf;   // CXL BufferFrame pointer for O(1) promotion path
    // NOTE: sticky uniform_or_skew flag removed. Skew is now derived on-the-fly inside
    // CheckAndPromote by scanning per-slot access counts against the current
    // per_page_visits * skew_threshold_ratio, so that a single early-window observation
    // can no longer permanently mask this candidate from Condition 1/2 promotion paths.

    std::atomic<u64> distinct_accessed_slot_set[3];            // Trakcs which slots have been accessed
    std::atomic<u64> already_promoted_slot_set[3];              // Tracks which slots have been promoted

    // Admission and deletion parameter
    std::atomic<u64> per_hot_page_visit_count;          // incremented on each access after admission
    // NOTE: per_hot_page_requests_snapshot removed: epoch-timeout policy clears all
    // candidates together at each histogram-aging tick, so per-candidate admission
    // timestamp is no longer needed.


    // Constructor
    HotPageCandidate(u64 page_cms_visit_count,
                     u16 page_slot_num,
                     leanstore::storage::BufferFrame* cxl_bf)
        :   page_cms_visit_count(page_cms_visit_count),
            page_slot_num(page_slot_num),
            cxl_bf(cxl_bf),
            per_hot_page_visit_count(0)
    {
        assert(page_slot_num > 0 && page_slot_num <= MAX_SLOTS_PER_PAGE);
        for(int i = 0; i < 3; i++){
            distinct_accessed_slot_set[i].store(0, std::memory_order_relaxed);
            already_promoted_slot_set[i].store(0, std::memory_order_relaxed);
        }
    }

    // Move Constructor,
    // because we want to put HotPageCandidate into std::unordered_map<page_id, HotPageCandidate>
    // std::atomic<bool> and std::mutex can not be copyed or moved
    HotPageCandidate(HotPageCandidate&& other) noexcept
    : page_cms_visit_count(other.page_cms_visit_count),
      page_slot_num(other.page_slot_num),
      cxl_bf(other.cxl_bf),
      per_hot_page_visit_count(other.per_hot_page_visit_count.load(std::memory_order_relaxed))
    {
        // loading atomic array
        for(int i = 0; i < 3; i++){
            distinct_accessed_slot_set[i].store(
                other.distinct_accessed_slot_set[i].load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            already_promoted_slot_set[i].store(
                other.already_promoted_slot_set[i].load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
    }

    // Disable copy constructor and assignment
    HotPageCandidate(const HotPageCandidate&) = delete;
    HotPageCandidate &operator=(const HotPageCandidate&) = delete;
    HotPageCandidate &operator=(const HotPageCandidate&&) = delete;

    // Get the number of distinct slots accessed so far
    // it is easy using std::bitset, just call .count()
    // for fast-acquire, using std::atomic<u64>
    u64 GetDistinctAccessedSlotCount() const {
        return __builtin_popcountll(distinct_accessed_slot_set[0].load(std::memory_order_relaxed)) +
               __builtin_popcountll(distinct_accessed_slot_set[1].load(std::memory_order_relaxed)) +
               __builtin_popcountll(distinct_accessed_slot_set[2].load(std::memory_order_relaxed));
    }

    // Get the number of already promoted slots
    u64 GetAlreadyPromotedSlotCount() const{
        return __builtin_popcountll(already_promoted_slot_set[0].load(std::memory_order_relaxed)) +
               __builtin_popcountll(already_promoted_slot_set[1].load(std::memory_order_relaxed)) +
               __builtin_popcountll(already_promoted_slot_set[2].load(std::memory_order_relaxed));
    }

    // Check if a specific slot has been accessed
    bool IsSlotAccessed(u16 slot_id) const{
        assert(slot_id < page_slot_num);
        return (distinct_accessed_slot_set[slot_id / 64].load(std::memory_order_relaxed) >> (slot_id % 64)) & 1;
    }

    // Check if a specific slot has been promoted
    bool IsSlotPromoted(u16 slot_id) const {
        assert(slot_id < page_slot_num);
        return (already_promoted_slot_set[slot_id / 64].load(std::memory_order_relaxed) >> (slot_id % 64)) & 1;
    }

    // Worker thread calling(heavily-write-path)
    void MarkSlotAsAccessed(u16 slot_id){
        assert(slot_id < page_slot_num);
        distinct_accessed_slot_set[slot_id / 64].fetch_or(1ULL << (slot_id % 64), std::memory_order_relaxed);
    }

    // Worker thread calling(heavily-write-path)
    void MarkSlotAsPromoted(u16 slot_id){
        assert(slot_id < page_slot_num);
        already_promoted_slot_set[slot_id / 64].fetch_or(1ULL << (slot_id % 64), std::memory_order_relaxed);
    }

    void ClearPromotedSlotBit(u16 slot_id){
        assert(slot_id < page_slot_num);
        already_promoted_slot_set[slot_id / 64].fetch_and(~(1ULL << (slot_id % 64)), std::memory_order_relaxed);
    }

    // NOTE: MarkAsSkew() / IsSkew() were removed along with the sticky uniform_or_skew flag.
    // Skew is now evaluated on-the-fly inside CheckAndPromote() so that stale early-window
    // observations can no longer permanently mask a page from Condition 1/2 promotion.

    // Increment per-page visit counter
    void IncrementVisitCount(){
        per_hot_page_visit_count.fetch_add(1, std::memory_order_relaxed);
    }

    // Get per-page visit counter
    u64 GetPerPageVisitCount() const {
        return per_hot_page_visit_count.load(std::memory_order_relaxed);
    }
};

//--------------------------[Added.]---------------------------------------------------------
// DramHotPageCandidates: Using std::unordered_map<page_id, HotPageCandidates> 
// the second-level filtering
// dram_hot_page_candidates lifecycle:
//
// [Admission]:
// (1) During streaming page access, if exceeding threshold, add to candidates
// (2) After histogram update, scan depulicate_sets that has pages excedding threshold and add them
//
// [Deletion]:
// (1) If per_hot_page_visit_count >= FLAGS_max_per_page_visits and not detected as skew, promote entire page
// (2) If per_hot_page_visit_count < FLAGS_max_per_page_visits but detected as skew, promote skew records upon detection
// (3) Epoch-timeout: clear all candidates together on every histogram-aging tick.
// 
// [Aging]:
// RecordCMS aging is triggered using the same request window as sampled histogram updates:
// FLAGS_trigger_visit_histogram_update_size.
//-------------------------------------------------------------------------------------------
class DramHotPageCandidates{
private:
    static inline void print_info(const std::string& msg){
        std::cout << msg << "\n";
    }

    // Sharded candidate storage: split by page_id to reduce cross-page contention.
    static constexpr u16 NUM_CANDIDATE_SHARDS = 16;

    struct CandidateShard {
        std::unordered_map<u64, HotPageCandidate> map;
        mutable std::shared_mutex mutex;
    };

    std::array<CandidateShard, NUM_CANDIDATE_SHARDS> shards;

    static u16 getShardIndex(u64 page_id) {
        return static_cast<u16>(page_id % NUM_CANDIDATE_SHARDS);
    }

    // Using RecordCountMinSketch to track (page_id, slot_id) access counts
    RecordCountMinSketch &record_cms;


    std::atomic<u64> global_requests;           // Global request counter(tracks all page accesses, not just candidates)

    // Configuraton parameters(can be tuned via FLAGS, see Config.cpp)
    double skew_threshold_ratio;        // FLAGS_skew_threshold_ratio: if a single slot > x% of the total visits, consider skewed
    double uniform_threshold_ratio;     // FLAGS_uniform_threshold_ratio: if x%+ slots accessed without skew, consider uniform
    u64 max_per_page_visits;            // FLAGS_max_per_page_visits: must decide skew window-size
    u64 configured_max_per_page_visits; // immutable hard-coded baseline from flags
    u64 max_global_requests_window;     // kept for compatibility; epoch-timeout policy no longer uses per-candidate timeout

    // Dynamic per-page visits ceiling derived from workload parameters (working_set_gib + zipf_theta
    // + trigger_visit_histogram_update_size + DRAM-vs-CXL ratio). 0 means "not configured, ignore".
    // The effective Condition-1 threshold used in CheckAndPromote is min(configured, dynamic) when
    // dynamic > 0, otherwise falls back to configured.
    std::atomic<u64> dynamic_max_per_page_visits{0};

    // Diagnostics counters (cumulative)
    std::atomic<u64> skew_promote_count{0};      // number of skew-record promotions (hot slots)
    std::atomic<u64> uniform_promote_count{0};   // number of full-page promotions
    std::atomic<u64> timeout_removed_count{0};   // number of candidates removed due to timeout

    // Round counter for CheckAndPromote, used only to label [SUMMARY] log lines
    // so downstream analysis can compute per-round branch distributions.
    std::atomic<u64> check_and_promote_round_no{0};

public:
    // Constructor
    explicit DramHotPageCandidates(
        RecordCountMinSketch& record_cms,
        double skew_threshold_ratio,
        double uniform_threshold_ratio,
        u64 max_per_page_visits,
        u64 max_global_requests_window
    ): record_cms(record_cms),
    global_requests(0),
    skew_threshold_ratio(skew_threshold_ratio),
    uniform_threshold_ratio(uniform_threshold_ratio),
    max_per_page_visits(max_per_page_visits),
    configured_max_per_page_visits(max_per_page_visits),
    max_global_requests_window(max_global_requests_window){}

    //---------------------------------------------------------------------------------------------------
    // Helper function:
    // SkewRecordAdmission::OnPageAccess(u64 page_id)
    // the global_requests should increased on every page visit,
    // instead of on every Record visit
    // Call Usage:
    // Single-thread: SampledVisitHistogram::OnPageAccess() -> SkewRecordAdmission::OnPageAcess()
    // Multi-thread: SampledVisitHistogram::WorkerThreadOnPageAccess() -> SkewRecordAdmission::OnPageAccess()
    //-----------------------------------------------------------------------------------------------------
    void OnPageAccess(){
        global_requests.fetch_add(1, std::memory_order_relaxed);
    }

    size_t GetCandidatesSize() const {
        size_t total = 0;
        for (u16 i = 0; i < NUM_CANDIDATE_SHARDS; i++) {
            std::shared_lock<std::shared_mutex> lock(shards[i].mutex);
            total += shards[i].map.size();
        }
        return total;
    }

    bool Contains(u64 page_id) const {
        const u16 shard_idx = getShardIndex(page_id);
        std::shared_lock<std::shared_mutex> lock(shards[shard_idx].mutex);
        return shards[shard_idx].map.find(page_id) != shards[shard_idx].map.end();
    }

    //----------------------------------------------------------------------------------------
    // Helper function:
    // Add a page to the candidate pool(called after passing the first-level threshold)
    //
    // if successfully added, return true, else return false
    // after makes sure that PageCountMinSketch.CMSQuery(page_id) > SampledVisitHistogram.current_threshold_fine.load(std::memory_ordered_relaxed);
    // Once a page is admitted into DramHotPageCandidates, it has the snapshot value of page_cms_visit_count.
    // page_slot_num is obtained through higher-level BufferFrame calling context.
    // The caller is responsible for passing page_slot_num parameter.
    //
    // is_in_dram: whether `cxl_bf` currently resides in the DRAM buffer pool,
    // as seen by the caller (BufferManager::isInDRAM(cxl_bf)). It is passed in
    // rather than resolved here because SkewRecordAdmission.hpp is transitively
    // included from BufferManager.hpp, so BMC::global_bf is not yet visible at
    // this point in the include chain. The flag is only used for logging.
    bool AddCandidate(u64 page_id,
                      u64 page_cms_visit_count,
                      u16 page_slot_num,
                      leanstore::storage::BufferFrame* cxl_bf,
                      bool is_in_dram){
        const u16 shard_idx = getShardIndex(page_id);
        // unique_lock for write operation on the relevant shard only
        std::unique_lock<std::shared_mutex> lock(shards[shard_idx].mutex);
        // if already exists,
        if(shards[shard_idx].map.find(page_id) != shards[shard_idx].map.end()){
            return false;   // Already a candidate
        }

        // Under epoch-timeout policy there is no per-candidate admission timestamp,
        // so we no longer read global_requests here.
        shards[shard_idx].map.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(page_id),
            std::forward_as_tuple(page_cms_visit_count, page_slot_num, cxl_bf)
        );

        return true;
    }

    //-------------------------------------------------------------------------------------------
    // OnRecordAccess:
    // Always use blocking shared_lock (not try_lock) so candidate-side accounting
    // is not silently dropped under background-thread contention.
    //--------------------------------------------------------------------------------------------
    void OnRecordAccess(u64 page_id, u16 slot_id, leanstore::storage::BufferFrame* cxl_bf = nullptr){
        // 1. lock-free update RecordCMS
        record_cms.CMSRecordAccessUpdate(page_id, slot_id);

        // 2. shared_lock on the relevant shard only
        const u16 shard_idx = getShardIndex(page_id);
        std::shared_lock<std::shared_mutex> lock(shards[shard_idx].mutex);

        auto it = shards[shard_idx].map.find(page_id);
        if(it != shards[shard_idx].map.end()){
            HotPageCandidate& hot_page_candidate = it -> second;
            if(cxl_bf != nullptr){
                hot_page_candidate.cxl_bf = cxl_bf;
            }

            // Mark slot as accessed
            hot_page_candidate.MarkSlotAsAccessed(slot_id);

            // Increment per-page visit count
            hot_page_candidate.IncrementVisitCount();
        }
    }

    //-----------------------------------------------------------------------------------------------
    // ClearPromotedSlot:
    // Called by worker threads on the update path after invalidating a RecordCache entry.
    // Clears the promoted bit so that CheckAndPromote can re-promote the slot.
    // Defined in SkewRecordAdmission.cpp to avoid icache bloat in hot read TUs.
    //-----------------------------------------------------------------------------------------------
    void ClearPromotedSlot(u64 page_id, u16 slot_id);

    //-----------------------------------------------------------------------------------------------
    // CheckAndPromote:
    // Background thread function to check and promote candidates
    // This should be called periodically by a background thread
    // Retuns the vector of decisions <page_id, should_promote_entire_page, hot_slot_ids>
    //------------------------------------------------------------------------------------------------
    struct PromotionDecision{
        u64 page_id;
        leanstore::storage::BufferFrame* cxl_bf;
        bool promote_entire_page;
        std::vector<u16> hot_slot_ids;      // Useful when page is skew, promote_entire_page == false
    };

    // ---------------------------------------------------------------------------
    // getDynamicThreshold:
    // Linear-scaling formula that relaxes the admission threshold when
    // RecordCache is under-filled (warmup phase) and restores the original
    // threshold as the cache approaches capacity.
    //
    //   g(fillRatio) = alpha + (1 - alpha) * fillRatio
    //   result       = l1_page_threshold * g(fillRatio)
    //
    // Returns a double so that callers can decide their own rounding / floor
    // policy.  When fillRatio == 0 the result is l1_page_threshold * alpha
    // (most relaxed); when fillRatio == 1 the result equals l1_page_threshold
    // (original strictness).
    //
    // Default: alpha = 0.25
    // ---------------------------------------------------------------------------
    static double getDynamicThreshold(u64 l1_page_threshold,
                                      double record_cache_fill_ratio,
                                      double alpha = 0.25) {
        if (l1_page_threshold == 0) return 0.0;
        const double clamped_ratio = std::clamp(record_cache_fill_ratio, 0.0, 1.0);
        const double scale = alpha + (1.0 - alpha) * clamped_ratio;
        return static_cast<double>(l1_page_threshold) * scale;
    }

    // Lightweight snapshot of a HotPageCandidate for lock-free analysis.
    struct CandidateSnapshot {
        u64 page_id;
        leanstore::storage::BufferFrame* cxl_bf;
        u16 page_slot_num;
        u64 per_page_visits;
        u64 accessed_bits[3];
        u64 promoted_bits[3];

        bool IsSlotAccessed(u16 slot_id) const {
            return (accessed_bits[slot_id / 64] >> (slot_id % 64)) & 1;
        }
        bool IsSlotPromoted(u16 slot_id) const {
            return (promoted_bits[slot_id / 64] >> (slot_id % 64)) & 1;
        }
        u64 GetDistinctAccessedSlotCount() const {
            return __builtin_popcountll(accessed_bits[0]) +
                   __builtin_popcountll(accessed_bits[1]) +
                   __builtin_popcountll(accessed_bits[2]);
        }
        u64 GetAlreadyPromotedSlotCount() const {
            return __builtin_popcountll(promoted_bits[0]) +
                   __builtin_popcountll(promoted_bits[1]) +
                   __builtin_popcountll(promoted_bits[2]);
        }
    };

    std::vector<PromotionDecision> CheckAndPromote(u64 l1_fine_threshold,
                                                   double record_cache_fill_ratio = 0.0,
                                                   bool is_scan_workload = false) {
        // =================================================================
        // Phase 1: snapshot candidates shard-by-shard under per-shard
        // shared lock.  Each shard is locked briefly and independently,
        // so workers accessing pages in other shards are never blocked.
        // =================================================================
        std::vector<CandidateSnapshot> snapshots;
        snapshots.reserve(GetCandidatesSize());
        for (u16 shard_idx = 0; shard_idx < NUM_CANDIDATE_SHARDS; shard_idx++) {
            std::shared_lock<std::shared_mutex> lock(shards[shard_idx].mutex);
            for (const auto& [pid, cand] : shards[shard_idx].map) {
                CandidateSnapshot s;
                s.page_id        = pid;
                s.cxl_bf         = cand.cxl_bf;
                s.page_slot_num  = cand.page_slot_num;
                s.per_page_visits = cand.GetPerPageVisitCount();
                for (int i = 0; i < 3; i++) {
                    s.accessed_bits[i] = cand.distinct_accessed_slot_set[i].load(std::memory_order_relaxed);
                    s.promoted_bits[i] = cand.already_promoted_slot_set[i].load(std::memory_order_relaxed);
                }
                snapshots.push_back(s);
            }
        } // all shard locks released — heavy CMS work below is lock-free

        // =================================================================
        // Phase 2: analyse snapshots without any lock held.
        //
        // All CMS queries happen here, outside the critical section.
        // =================================================================
        std::vector<PromotionDecision> decisions;
        std::unordered_set<u64> to_remove;
        u64 local_skew_promote_count  = 0;
        u64 local_uniform_promote_count = 0;

        // Compute the fill-ratio-aware effective L1 threshold for Condition 0.
        const double dynamic_threshold_raw =
            getDynamicThreshold(l1_fine_threshold, record_cache_fill_ratio);
        const u64 effective_l1_fine_threshold =
            (l1_fine_threshold == 0) ? 0 : std::max<u64>(2, static_cast<u64>(dynamic_threshold_raw));

        const bool l1_rule_active = (effective_l1_fine_threshold > 0);

        // Resolve effective Condition-1 threshold once per invocation.
        const u64 dynamic_ceiling = dynamic_max_per_page_visits.load(std::memory_order_relaxed);
        const u64 effective_max_per_page_visits =
            (dynamic_ceiling > 0 && dynamic_ceiling < max_per_page_visits)
                ? dynamic_ceiling
                : max_per_page_visits;

        std::array<u64, MAX_SLOTS_PER_PAGE> slot_counts_buf{};

        for (const auto& snap : snapshots) {
            const u64 page_id          = snap.page_id;
            const u64 per_page_visits  = snap.per_page_visits;
            const u64 accessed_slots   = snap.GetDistinctAccessedSlotCount();
            const u64 uniform_slot_threshold =
                static_cast<u64>(snap.page_slot_num * uniform_threshold_ratio);

            // ------------------------------------------------------------------
            // Collect per-slot CMS counts for this page once.
            // Uses stack array and bitwise iteration to skip unaccessed slots.
            // ------------------------------------------------------------------
            std::fill(slot_counts_buf.begin(), slot_counts_buf.begin() + snap.page_slot_num, 0ULL);
            u64 cms_total = 0;
            for (int word = 0; word < 3; word++) {
                u64 bits = snap.accessed_bits[word];
                while (bits) {
                    const int bit = __builtin_ctzll(bits);
                    const u16 slot_id = static_cast<u16>(word * 64 + bit);
                    if (slot_id >= snap.page_slot_num) break;
                    const u64 cnt = record_cms.CMSGetRecordAccessCount(page_id, slot_id);
                    slot_counts_buf[slot_id] = cnt;
                    cms_total += cnt;
                    bits &= bits - 1;
                }
            }

            // Unified skew threshold: prefer CMS total, fall back to per_page_visits.
            const u64 reference_count = (cms_total > 0) ? cms_total : per_page_visits;
            const u64 skew_threshold = std::max<u64>(1, static_cast<u64>(std::ceil(reference_count * skew_threshold_ratio)));

            // ------------------------------------------------------------------
            // Condition 0 (highest priority): strong-skew by dynamic L1 threshold.
            // Uses bitwise iteration to only check slots with non-zero counts.
            // ------------------------------------------------------------------
            if (l1_rule_active && !is_scan_workload) {
                std::vector<u16> strong_hot_slots;
                for (int word = 0; word < 3; word++) {
                    u64 bits = snap.accessed_bits[word];
                    while (bits) {
                        const int bit = __builtin_ctzll(bits);
                        const u16 slot_id = static_cast<u16>(word * 64 + bit);
                        if (slot_id >= snap.page_slot_num) break;
                        if (slot_counts_buf[slot_id] >= effective_l1_fine_threshold &&
                            !snap.IsSlotPromoted(slot_id)) {
                            strong_hot_slots.push_back(slot_id);
                        }
                        bits &= bits - 1;
                    }
                }

                if (!strong_hot_slots.empty()) {
                    decisions.push_back({page_id, snap.cxl_bf, false, strong_hot_slots});
                    local_skew_promote_count += strong_hot_slots.size();
                    continue;
                }
            }

            // ------------------------------------------------------------------
            // Skew detection (shared by Condition 3).
            // Uses bitwise iteration to skip zero-count slots.
            // ------------------------------------------------------------------
            bool has_ratio_skew_slot = false;
            std::vector<u16> ratio_new_hot_slots;
            std::vector<u16> ratio_dup_hot_slots;

            for (int word = 0; word < 3; word++) {
                u64 bits = snap.accessed_bits[word];
                while (bits) {
                    const int bit = __builtin_ctzll(bits);
                    const u16 slot_id = static_cast<u16>(word * 64 + bit);
                    if (slot_id >= snap.page_slot_num) break;
                    const u64 cnt = slot_counts_buf[slot_id];
                    if (cnt > skew_threshold) {
                        has_ratio_skew_slot = true;
                        if (snap.IsSlotPromoted(slot_id)) {
                            ratio_dup_hot_slots.push_back(slot_id);
                        } else {
                            ratio_new_hot_slots.push_back(slot_id);
                        }
                    }
                    bits &= bits - 1;
                }
            }

            // ------------------------------------------------------------------
            // Condition 1: uniform promote by per_page_visits >= effective_max.
            // ------------------------------------------------------------------
            if (per_page_visits >= effective_max_per_page_visits && (!has_ratio_skew_slot || is_scan_workload)) {
                decisions.push_back({page_id, snap.cxl_bf, true, {}});
                to_remove.emplace(page_id);
                local_uniform_promote_count++;
                continue;
            }

            // ------------------------------------------------------------------
            // Condition 2: uniform promote by accessed_slots >= uniform_slot_threshold.
            // ------------------------------------------------------------------
            if (accessed_slots >= uniform_slot_threshold && (!has_ratio_skew_slot || is_scan_workload)) {
                decisions.push_back({page_id, snap.cxl_bf, true, {}});
                to_remove.emplace(page_id);
                local_uniform_promote_count++;
                continue;
            }

            // ------------------------------------------------------------------
            // Condition 3: ratio-based skew detected -> promote hot records only.
            // ------------------------------------------------------------------
            if (has_ratio_skew_slot) {
                if (!ratio_new_hot_slots.empty()) {
                    decisions.push_back({page_id, snap.cxl_bf, false, ratio_new_hot_slots});
                    local_skew_promote_count += ratio_new_hot_slots.size();
                }
                // Remove candidate if every accessed slot has now been promoted.
                if (snap.GetDistinctAccessedSlotCount() ==
                    snap.GetAlreadyPromotedSlotCount() + ratio_new_hot_slots.size()) {
                    to_remove.emplace(page_id);
                }
                continue;
            }

            // ------------------------------------------------------------------
            // Condition 4 (scan-mode only): fallback — any remaining candidate
            // that didn't match Conditions 1/2/3 is promoted as a full page.
            // ------------------------------------------------------------------
            if (is_scan_workload) {
                decisions.push_back({page_id, snap.cxl_bf, true, {}});
                to_remove.emplace(page_id);
                local_uniform_promote_count++;
                continue;
            }
        }

        // =================================================================
        // Phase 3: apply write-back shard-by-shard under brief per-shard
        // exclusive lock.  Pre-bucketed by shard to avoid redundant
        // getShardIndex checks and skip empty shards entirely.
        // =================================================================
        std::array<std::vector<u64>, NUM_CANDIDATE_SHARDS> remove_by_shard;
        for (u64 pid : to_remove)
            remove_by_shard[getShardIndex(pid)].push_back(pid);

        std::array<std::vector<const PromotionDecision*>, NUM_CANDIDATE_SHARDS> decisions_by_shard;
        for (const auto& d : decisions) {
            if (!d.promote_entire_page)
                decisions_by_shard[getShardIndex(d.page_id)].push_back(&d);
        }

        for (u16 shard_idx = 0; shard_idx < NUM_CANDIDATE_SHARDS; shard_idx++) {
            if (remove_by_shard[shard_idx].empty() && decisions_by_shard[shard_idx].empty()) continue;
            std::unique_lock<std::shared_mutex> lock(shards[shard_idx].mutex);

            for (const auto* d : decisions_by_shard[shard_idx]) {
                auto it = shards[shard_idx].map.find(d->page_id);
                if (it != shards[shard_idx].map.end())
                    for (u16 slot_id : d->hot_slot_ids)
                        it->second.MarkSlotAsPromoted(slot_id);
            }

            for (u64 pid : remove_by_shard[shard_idx])
                shards[shard_idx].map.erase(pid);
        } // all shard exclusive locks released

        skew_promote_count.fetch_add(local_skew_promote_count,   std::memory_order_relaxed);
        uniform_promote_count.fetch_add(local_uniform_promote_count, std::memory_order_relaxed);

        check_and_promote_round_no.fetch_add(1, std::memory_order_relaxed);

        return decisions;
    }
    // Scan-mode epoch helper: collect all remaining candidates as full-page promotions
    // before ClearAllCandidatesByEpoch() wipes them.
    std::vector<PromotionDecision> CollectAllAsFullPagePromotions() {
        std::vector<PromotionDecision> decisions;
        for (u16 shard_idx = 0; shard_idx < NUM_CANDIDATE_SHARDS; shard_idx++) {
            std::shared_lock<std::shared_mutex> lock(shards[shard_idx].mutex);
            for (const auto& [pid, cand] : shards[shard_idx].map) {
                decisions.push_back({pid, cand.cxl_bf, true, {}});
            }
        }
        return decisions;
    }

    // Epoch-timeout policy:
    // when page/record CMS aging tick happens, clear all candidates together.
    u64 ClearAllCandidatesByEpoch(){
        u64 removed = 0;
        for (u16 shard_idx = 0; shard_idx < NUM_CANDIDATE_SHARDS; shard_idx++) {
            std::unique_lock<std::shared_mutex> lock(shards[shard_idx].mutex);
            removed += shards[shard_idx].map.size();
            shards[shard_idx].map.clear();
        }
        timeout_removed_count.fetch_add(removed, std::memory_order_relaxed);
        return removed;
    }


    //------------------------------------------------------------------------------------------------------------------
    // getters / setters
    //------------------------------------------------------------------------------------------------------------------
    void SetSkewThresholdRatio(double ratio) {skew_threshold_ratio = ratio;}

    void SetUniformThresholdRatio(double ratio){ uniform_threshold_ratio = ratio;}

    void SetMaxPerPageVisit(u64 visits) {max_per_page_visits = visits;}
    void SetMaxGlobalRequestsWindow(u64 window) {max_global_requests_window = window; }

    // Set the dynamic per-page-visits ceiling derived from workload characteristics.
    // Passing 0 disables the dynamic ceiling and falls back to configured max only.
    void SetDynamicMaxPerPageVisits(u64 visits){
        dynamic_max_per_page_visits.store(visits, std::memory_order_relaxed);
    }

    double GetSkewThresholdRatio() const { return skew_threshold_ratio;}
    double GetUniformThresholdRatio() const {return uniform_threshold_ratio;}
    u64 GetMaxPerPageVisits() const {return max_per_page_visits;}
    u64 GetConfiguredMaxPerPageVisits() const {return configured_max_per_page_visits;}
    u64 GetDynamicMaxPerPageVisits() const {
        return dynamic_max_per_page_visits.load(std::memory_order_relaxed);
    }
    u64 GetEffectiveMaxPerPageVisits() const {
        u64 dyn = dynamic_max_per_page_visits.load(std::memory_order_relaxed);
        if(dyn > 0 && dyn < max_per_page_visits){
            return dyn;
        }
        return max_per_page_visits;
    }
    u64 GetMaxGloablRequestWindow() const {return max_global_requests_window;}

    //--------------------------------------------------------------------------------------------
    // Static helper: ComputeDynamicMaxPerPageVisits
    //
    // Purpose:
    //   Estimate a per-page-visits ceiling that represents the average visit count on the
    //   DRAM-admissible top-x% pages within ONE observation window
    //   (= FLAGS_trigger_visit_histogram_update_size), given:
    //     - total working-set size (GiB)
    //     - Zipfian skew parameter theta
    //     - fraction of pages that DRAM+RecordCache can hold (= GetDRAMvsCXLRatio)
    //     - BTree page size (bytes)
    //
    // Semantics:
    //   We first derive top-x% pages that DRAM can keep from capacity:
    //      x = dram_ratio, N_dram = floor(N_total * x).
    //   Under Zipf(theta), these top-N_dram pages receive p_dram probability mass.
    //   The dynamic ceiling is the *average* visits per admitted page in one
    //   trigger window:
    //
    //      per_page_visits_ceiling = (trigger_window * p_dram) / N_dram
    //
    //   This directly matches Condition-1 intent: "for pages that should be in DRAM
    //   (top x%), what is the expected per-page visit count in one histogram window?"
    //
    // Math:
    //   Let N_total = working_set_gib * GiB / page_size_bytes
    //       N_dram  = floor(N_total * dram_ratio)                  (top-N_dram pages)
    //       H(k, t) = sum_{i=1..k} 1 / i^t                         (generalized harmonic number)
    //       p_dram  = H(N_dram, t) / H(N_total, t)                 (prob mass on top-x% pages)
    //       per_page_visits_ceiling = (trigger_window * p_dram) / N_dram
    //
    // Worked example (working_set=2GiB, page=16KiB, theta=0.9, dram_ratio=0.2, window=1M):
    //   N_total ~= 131072, N_dram ~= 26214, p_dram ~= 0.79
    //   per_page_visits ~= (1,000,000 * 0.79) / 26,214 ~= 30.
    //
    // Notes:
    //   - Returns 0 when any input is non-positive (caller should treat as "disabled").
    //   - For performance, we do a single pass to compute H(N_total) and H(N_dram),
    //     O(N_total) which is fine for YCSB-small workloads.
    //--------------------------------------------------------------------------------------------
    // Euler-Maclaurin approximation of the generalized harmonic number H(n, theta).
    // Replaces O(N) exact summation with O(1) closed-form; error < 1e-6 for n >= 1.
    static double ApproxHarmonic(u64 n, double theta) {
        if (n == 0) return 0.0;
        if (std::abs(theta - 1.0) < 1e-9)
            return std::log(static_cast<double>(n)) + 0.5772156649;
        const double nd = static_cast<double>(n);
        return std::pow(nd, 1.0 - theta) / (1.0 - theta)
             + 0.5 * std::pow(nd, -theta)
             + 1.0;
    }

    static u64 ComputeDynamicMaxPerPageVisits(double working_set_gib,
                                              double zipf_theta,
                                              u64 trigger_window,
                                              double dram_ratio,
                                              u64 page_size_bytes = 16 * 1024){
        if(working_set_gib <= 0.0 || trigger_window == 0 || dram_ratio <= 0.0 || page_size_bytes == 0){
            return 0;
        }
        constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
        const u64 total_pages = static_cast<u64>((working_set_gib * GiB) / static_cast<double>(page_size_bytes));
        if(total_pages == 0){
            return 0;
        }
        u64 dram_pages = static_cast<u64>(static_cast<double>(total_pages) * dram_ratio);
        if(dram_pages == 0){
            dram_pages = 1;
        }
        if(dram_pages > total_pages){
            dram_pages = total_pages;
        }

        const double h_total = ApproxHarmonic(total_pages, zipf_theta);
        const double h_dram  = ApproxHarmonic(dram_pages, zipf_theta);
        if(h_total <= 0.0 || h_dram <= 0.0){
            return 0;
        }

        const double p_dram = h_dram / h_total;
        const double visits_on_dram_pages = static_cast<double>(trigger_window) * p_dram;
        const double per_page_visits      = visits_on_dram_pages / static_cast<double>(dram_pages);

        if(per_page_visits < 1.0){
            return 1;
        }
        return static_cast<u64>(std::ceil(per_page_visits));
    }

    u64 GetGlobalRequests() const {return global_requests.load(std::memory_order_relaxed);}
    u64 GetSkewPromoteCount() const {return skew_promote_count.load(std::memory_order_relaxed);}
    u64 GetUniformPromoteCount() const {return uniform_promote_count.load(std::memory_order_relaxed);}
    u64 GetTimeoutRemovedCount() const {return timeout_removed_count.load(std::memory_order_relaxed);}

};

}