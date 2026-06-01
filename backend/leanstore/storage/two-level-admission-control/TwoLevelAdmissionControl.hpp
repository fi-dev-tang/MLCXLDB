#pragma once
#include "CountMinSketch.hpp"
#include "DepulicateSetForPageSampling.hpp"
#include "SampledVisitHistogram.hpp"
#include "VisitCountBucket.hpp"
#include "SkewRecordAdmission.hpp"
#include "Units.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/storage/bf-tree/BFTreeAdmissionPolicy.hpp"
#include "leanstore/storage/hybried-tier-asplos2025/HybridTierASPLOS2025AdmissionPolicy.hpp"
#include <cstdio>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace leanstore::storage::two_level_admission_control {

// The exposed interface of two_level_admission_control
class TwoLevelAdmissionControl {
private:
    // Level 1: Page-level admission components
    PageCountMinSketch& page_cms;                           
    SampledVisitHistogram& page_visit_histogram;            

    // Level 2: Record-level skew detection components
    RecordCountMinSketch& record_cms;                       
    DramHotPageCandidates& dram_hot_page_candidates;        

    struct LRUCandidate {
        u64 page_id;
        leanstore::storage::BufferFrame* cxl_bf;
    };
    std::list<LRUCandidate> lru_candidates;
    std::unordered_map<u64, std::list<LRUCandidate>::iterator> lru_index;
    std::mutex lru_mutex;

    std::list<LRUCandidate> page_only_candidates;
    std::unordered_map<u64, std::list<LRUCandidate>::iterator> page_only_index;
    std::mutex page_only_mutex;

    // Cached RecordCache fill ratio, updated by BackgroundRoutine periodically.
    // Worker threads read this atomically on the hot path to avoid calling
    // getUsageRatio() on every access.
    std::atomic<double> cached_fill_ratio{0.0};

    // Comparison baseline policies (lazy-initialized on first use)
    std::unique_ptr<bf_tree::BFTreeAdmissionPolicy> bf_tree_policy_;
    std::unique_ptr<hybried_tier_asplos2025::HybridTierASPLOS2025AdmissionPolicy> hybrid_tier_asplos2025_policy_;

    bf_tree::BFTreeAdmissionPolicy& getBFTreePolicy() {
        if (!bf_tree_policy_)
            bf_tree_policy_ = std::unique_ptr<bf_tree::BFTreeAdmissionPolicy>(
                new bf_tree::BFTreeAdmissionPolicy(page_cms, page_visit_histogram, dram_hot_page_candidates));
        return *bf_tree_policy_;
    }
    hybried_tier_asplos2025::HybridTierASPLOS2025AdmissionPolicy& getHybridTierPolicy() {
        if (!hybrid_tier_asplos2025_policy_)
            hybrid_tier_asplos2025_policy_ = std::unique_ptr<hybried_tier_asplos2025::HybridTierASPLOS2025AdmissionPolicy>(
                new hybried_tier_asplos2025::HybridTierASPLOS2025AdmissionPolicy(page_visit_histogram, dram_hot_page_candidates));
        return *hybrid_tier_asplos2025_policy_;
    }

public:
    // Constructor to initialize references
    TwoLevelAdmissionControl(
        PageCountMinSketch& page_cms,
        SampledVisitHistogram& page_visit_histogram,
        RecordCountMinSketch& record_cms,
        DramHotPageCandidates& dram_hot_page_candidates
    ) : page_cms(page_cms),
        page_visit_histogram(page_visit_histogram),
        record_cms(record_cms),
        dram_hot_page_candidates(dram_hot_page_candidates) {}

    // Eager-init entry point. Called by the Wrapper ctor (single-threaded,
    // FLAGS already parsed) to allocate the baseline policy on the main
    // thread before any worker / admission thread starts. Avoids racing
    // the first OnRecordAccess malloc with admission-thread startup
    // (CPUCounters::registerThread + Worker ctor), which was the D6 hang.
    void EagerInitBaselinePolicy() {
        if (FLAGS_admission_mode == "bf-tree") {
            (void)getBFTreePolicy();
        } else if (FLAGS_admission_mode == "hybried-tier-asplos2025") {
            (void)getHybridTierPolicy();
        }
    }

    //-------------------------------------------------------------------------
    // Worker Thread Interface (Critical Path)
    // Called whenever a record is accessed.
    //-------------------------------------------------------------------------
    void OnRecordAccess(u64 page_id,
                        u16 slot_id,
                        bool is_in_dram,
                        u16 page_slot_num = 175,
                        leanstore::storage::BufferFrame* cxl_bf = nullptr) {
        // Comparison baseline dispatch (must come before is_in_dram check — baselines need DRAM info)
        if (FLAGS_admission_mode == "bf-tree") {
            getBFTreePolicy().OnRecordAccess(page_id, slot_id, is_in_dram, page_slot_num, cxl_bf);
            return;
        }
        // tiered-indexing-zxj (3T Two-Tree) is handled at the frontend level
        // with two physical B+Trees — it does not go through this admission path.
        if (FLAGS_admission_mode == "tiered-indexing-zxj") {
            return;
        }
        if (FLAGS_admission_mode == "hybried-tier-asplos2025") {
            getHybridTierPolicy().OnRecordAccess(page_id, slot_id, is_in_dram, page_slot_num, cxl_bf);
            return;
        }

        // If Page already in dram, no need to go through the cxl -> dram admission logic
        if (is_in_dram) {
            return;
        }

        if (FLAGS_admission_mode == "lru") {
            std::lock_guard<std::mutex> guard(lru_mutex);
            auto it = lru_index.find(page_id);
            if (it != lru_index.end()) {
                it->second->cxl_bf = cxl_bf;
                lru_candidates.splice(lru_candidates.begin(), lru_candidates, it->second);
            } else {
                lru_candidates.push_front({page_id, cxl_bf});
                lru_index[page_id] = lru_candidates.begin();
            }
            return;
        }

        if (FLAGS_admission_mode == "page_only") {
            // Level-1 only: update page-side stats and use threshold decision,
            // then enqueue full-page promotion candidate without RecordCMS logic.
            page_visit_histogram.WorkerThreadOnPageAccess(page_id);
            u64 page_admission_threshold = page_visit_histogram.GetAdmissionThreshold_fine();
            u64 current_page_visit_count = page_cms.CMSGetPageAccessCount(page_id);
            if (page_admission_threshold > 1 && current_page_visit_count >= page_admission_threshold) {
                std::lock_guard<std::mutex> guard(page_only_mutex);
                auto it = page_only_index.find(page_id);
                if (it != page_only_index.end()) {
                    it->second->cxl_bf = cxl_bf;
                    page_only_candidates.splice(page_only_candidates.begin(), page_only_candidates, it->second);
                } else {
                    page_only_candidates.push_front({page_id, cxl_bf});
                    page_only_index[page_id] = page_only_candidates.begin();
                }
            }
            return;
        }

        // ====================================================================
        // Step 1: Level 1 Admission (Page-Level)
        // ====================================================================
        
        // (1.1) Update Page CMS and sampling set (Internal logic handles 1w trigger)
        page_visit_histogram.WorkerThreadOnPageAccess(page_id);
        
        // (1.2) Update global request counter (epoch tick handled in background path)
        dram_hot_page_candidates.OnPageAccess();

        // (1.3) Check if the page passes the dynamic threshold.
        //       The raw L1 fine threshold is scaled by RecordCache fill ratio
        //       via getDynamicThreshold so that warmup is more aggressive when
        //       the cache is still mostly empty.
        const u64 raw_page_threshold = page_visit_histogram.GetAdmissionThreshold_fine();
        const double current_fill = cached_fill_ratio.load(std::memory_order_relaxed);
        const double dynamic_page_threshold_raw =
            DramHotPageCandidates::getDynamicThreshold(raw_page_threshold, current_fill);
        const u64 page_admission_threshold =
            (raw_page_threshold == 0) ? 0 : std::max<u64>(2, static_cast<u64>(dynamic_page_threshold_raw));
        u64 current_page_visit_count = page_cms.CMSGetPageAccessCount(page_id);

        // ====================================================================
        // Step 2: Level 2 Admission (Record-Level Skew Detection)
        // ====================================================================
        
        const bool pass_page_threshold = (page_admission_threshold > 1 && current_page_visit_count >= page_admission_threshold);
        if (pass_page_threshold) {
            // (2.1) Try to add to candidates. If it's already there, this returns false.
            dram_hot_page_candidates.AddCandidate(page_id, current_page_visit_count, page_slot_num, cxl_bf, is_in_dram);
        }

        // (2.2) Keep tracking record access for already-admitted pages even if
        // the page is not above the dynamic threshold on this access.
        // Otherwise the candidate may keep per-page timeout progress but lose
        // slot-level updates, making skew/uniform decisions stale.
        if (pass_page_threshold || dram_hot_page_candidates.Contains(page_id)) {
            // Note: SkewRecordAdmission::OnRecordAccess internally handles:
            // - Updating RecordCMS
            // - Incrementing candidate's per_hot_page_visit_count
            // - Checking if current slot visit count > skew_threshold
            // - Marking candidate as skew if necessary
            dram_hot_page_candidates.OnRecordAccess(page_id, slot_id, cxl_bf);
        }
    }

    //-------------------------------------------------------------------------
    // Worker Thread Interface (Update Path)
    // Called after a record is invalidated from RecordCache due to update.
    // Clears the promoted bit so the slot can be re-promoted.
    //-------------------------------------------------------------------------
    void OnRecordInvalidated(u64 page_id, u16 slot_id) {
        dram_hot_page_candidates.ClearPromotedSlot(page_id, slot_id);
    }

    //-------------------------------------------------------------------------
    // Background Thread Interface (Heavy Path)
    // Called periodically by a dedicated background thread.
    //-------------------------------------------------------------------------
    std::vector<DramHotPageCandidates::PromotionDecision> BackgroundRoutine(double record_cache_fill_ratio = 0.0) {
        // Update cached fill ratio so worker threads can use it on the hot path
        // without calling getUsageRatio() on every access.
        cached_fill_ratio.store(record_cache_fill_ratio, std::memory_order_relaxed);

        if (FLAGS_admission_mode == "lru") {
            std::vector<DramHotPageCandidates::PromotionDecision> decisions;
            decisions.reserve(FLAGS_lru_background_promote_batch);
            std::lock_guard<std::mutex> guard(lru_mutex);
            u64 promoted = 0;
            while (promoted < FLAGS_lru_background_promote_batch && !lru_candidates.empty()) {
                auto candidate = lru_candidates.front();
                lru_candidates.pop_front();
                lru_index.erase(candidate.page_id);
                decisions.push_back({candidate.page_id, candidate.cxl_bf, DramHotPageCandidates::PromotionDecision::Action::PROMOTE_FULL_PAGE, {}});
                promoted++;
            }
            return decisions;
        }

        if (FLAGS_admission_mode == "page_only") {
            page_visit_histogram.BackgroundThreadTryUpdate();
            std::vector<DramHotPageCandidates::PromotionDecision> decisions;
            decisions.reserve(FLAGS_lru_background_promote_batch);
            std::lock_guard<std::mutex> guard(page_only_mutex);
            u64 promoted = 0;
            while (promoted < FLAGS_lru_background_promote_batch && !page_only_candidates.empty()) {
                auto candidate = page_only_candidates.front();
                page_only_candidates.pop_front();
                page_only_index.erase(candidate.page_id);
                decisions.push_back({candidate.page_id, candidate.cxl_bf, DramHotPageCandidates::PromotionDecision::Action::PROMOTE_FULL_PAGE, {}});
                promoted++;
            }
            return decisions;
        }

        // Comparison baseline dispatch
        if (FLAGS_admission_mode == "bf-tree") {
            return getBFTreePolicy().BackgroundRoutine();
        }
        if (FLAGS_admission_mode == "tiered-indexing-zxj") {
            return {};  // 3T handled at frontend level
        }
        if (FLAGS_admission_mode == "hybried-tier-asplos2025") {
            return getHybridTierPolicy().BackgroundRoutine();
        }

        // 1. Try to update L1 histogram. If update happens, this marks an epoch boundary.
        const bool epoch_tick = page_visit_histogram.BackgroundThreadTryUpdate();

        // 2. Check candidate pool and generate decisions before epoch cleanup.
        //    Pass the current L1 fine threshold so that CheckAndPromote can apply a
        //    strong-skew rule: any single slot whose RecordCMS count has itself reached
        //    the page-level admission threshold is promoted immediately as a skew record,
        //    taking priority over the per_page_visits*ratio skew rule.
        const u64 l1_fine_threshold = page_visit_histogram.GetAdmissionThreshold_fine();
        auto decisions = dram_hot_page_candidates.CheckAndPromote(l1_fine_threshold, record_cache_fill_ratio, FLAGS_admission_scan_mode);

        // 3. Epoch boundary operations happen together:
        //    - record_cms aging
        //    - clear all hot-page candidates (global timeout policy)
        if (epoch_tick) {
            if (FLAGS_admission_scan_mode) {
                auto timeout_decisions = dram_hot_page_candidates.CollectAllAsFullPagePromotions();
                decisions.insert(decisions.end(), timeout_decisions.begin(), timeout_decisions.end());
            }
            record_cms.CMSAging();
            u64 cleared_candidates = dram_hot_page_candidates.ClearAllCandidatesByEpoch();
            std::fprintf(stdout,
                         "[DEBUG] admission_epoch_tick req=%llu l1_fine_threshold=%llu "
                         "promotions=%zu cleared_candidates=%llu\n",
                         static_cast<unsigned long long>(dram_hot_page_candidates.GetGlobalRequests()),
                         static_cast<unsigned long long>(page_visit_histogram.GetAdmissionThreshold_fine()),
                         decisions.size(),
                         static_cast<unsigned long long>(cleared_candidates));
        }
        return decisions;
    }
};



// Wrapper class to encapsulate all components for a single partition
class TwoLevelAdmissionControlWrapper {
private:
    PageCountMinSketch page_cms;
    VisitFrequencyBucketArray bucket_array;
    DepulicateSetForPageSampling sampling_set;
    SampledVisitHistogram page_visit_histogram;
    RecordCountMinSketch record_cms;
    DramHotPageCandidates dram_hot_page_candidates;
    TwoLevelAdmissionControl admission_control;

    // Page-CMS col_num auto-resolution:
    //   total_pages = (cxl_gib + dram_bp + dram_rc) GiB / 16 KiB
    //   col_num     = clamp(total_pages * alpha, 256K, 8M)
    // Rationale: the sketch only needs to track page_ids that participate in
    // admission -- i.e. anything resident in CXL or DRAM. SSD-only pages are
    // faulted into CXL first, so they enter the sketch then.
    // alpha=4 keeps avg-IDs-per-column near 1 (12 hash rows take min).
    // FLAGS_page_cms_col_num is now only the fallback when backend FLAGS
    // aren't populated yet (some unit tests run without parsing flags).
    static u64 ResolvePageCMSColNum() {
        constexpr u64    page_size_bytes = 16ULL * 1024;
        constexpr u64    min_col         = 256ULL * 1024;
        constexpr u64    max_col         = 8ULL  * 1024 * 1024;
        constexpr double alpha           = 4.0;
        const double cached_gib = FLAGS_cxl_gib
                                + FLAGS_dram_buffer_pool_gib
                                + FLAGS_dram_recordcache_gib;
        if (cached_gib <= 0.0) {
            return FLAGS_page_cms_col_num;
        }
        const u64 total_pages =
            static_cast<u64>(cached_gib * 1024.0 * 1024.0 * 1024.0) / page_size_bytes;
        u64 col = static_cast<u64>(static_cast<double>(total_pages) * alpha);
        if (col < min_col) col = min_col;
        if (col > max_col) col = max_col;
        return col;
    }

public:
    TwoLevelAdmissionControlWrapper()
        : page_cms(FLAGS_page_cms_row_num, ResolvePageCMSColNum()),
          bucket_array(FLAGS_visit_histogram_bucket_num),
          sampling_set(FLAGS_max_sampled_page_ids, FLAGS_trigger_visit_histogram_update_size),
          page_visit_histogram(page_cms, sampling_set, bucket_array),
          record_cms(FLAGS_record_cms_row_num, FLAGS_record_cms_col_num),
          dram_hot_page_candidates(
              record_cms,
              FLAGS_skew_threshold_ratio,
              FLAGS_uniform_threshold_ratio,
              FLAGS_max_per_page_visits,
              FLAGS_max_global_requests_window),
          admission_control(page_cms, page_visit_histogram, record_cms, dram_hot_page_candidates) {
        // Force baseline-policy construction here, on the single
        // BufferManager-ctor thread, so the first lazy malloc cannot race
        // with admission-thread startup. No-op for other admission_modes.
        admission_control.EagerInitBaselinePolicy();

        std::fprintf(stdout,
                     "[DEBUG] page_cms_sizing rows=%llu cols=%llu memory_mib=%.1f "
                     "derived_from_cached_gib=%.3f (cxl=%.3f bp=%.3f rc=%.3f)\n",
                     static_cast<unsigned long long>(FLAGS_page_cms_row_num),
                     static_cast<unsigned long long>(page_cms.CMSGetColNum()),
                     static_cast<double>(page_cms.CMSGetMemoryUsage()) / (1024.0 * 1024.0),
                     FLAGS_cxl_gib + FLAGS_dram_buffer_pool_gib + FLAGS_dram_recordcache_gib,
                     FLAGS_cxl_gib,
                     FLAGS_dram_buffer_pool_gib,
                     FLAGS_dram_recordcache_gib);
        std::fflush(stdout);
    }

    // Expose the core interface
    TwoLevelAdmissionControl& GetAdmissionControl() {
        return admission_control;
    }

    // Diagnostic accessors for testing and profiling
    PageCountMinSketch& GetPageCMS() { return page_cms; }
    RecordCountMinSketch& GetRecordCMS() { return record_cms; }
    SampledVisitHistogram& GetHistogram() { return page_visit_histogram; }
    DramHotPageCandidates& GetHotPageCandidates() { return dram_hot_page_candidates; }

    //---------------------------------------------------------------------------------------------
    // ConfigureDynamicMaxPerPageVisits:
    // Given the workload descriptor (working_set_gib + zipf_theta), compute a dynamic Condition-1
    // ceiling based on the current observation window FLAGS_trigger_visit_histogram_update_size and
    // the current DRAM-vs-CXL capacity ratio (from SampledVisitHistogram::GetDRAMvsCXLRatio).
    // Both the hard-coded FLAGS_max_per_page_visits and the newly-computed dynamic value are printed.
    // The effective threshold used by CheckAndPromote will be min(configured, dynamic).
    //---------------------------------------------------------------------------------------------
    // page_size_bytes default to 16 KiB which matches leanstore::storage::PAGE_SIZE.
    // We avoid including BufferFrame.hpp here to keep the admission-control header standalone.
    void ConfigureDynamicMaxPerPageVisits(double working_set_gib,
                                          double zipf_theta,
                                          u64 page_size_bytes = 16 * 1024){
        const double dram_ratio = page_visit_histogram.GetDRAMvsCXLRatio();
        const u64 trigger_window = FLAGS_trigger_visit_histogram_update_size;
        const u64 configured = FLAGS_max_per_page_visits;

        const u64 dynamic = DramHotPageCandidates::ComputeDynamicMaxPerPageVisits(
            working_set_gib, zipf_theta, trigger_window, dram_ratio, page_size_bytes);

        dram_hot_page_candidates.SetDynamicMaxPerPageVisits(dynamic);

        const u64 effective = (dynamic > 0 && dynamic < configured) ? dynamic : configured;
        std::fprintf(stdout,
                     "[DEBUG] dynamic_max_per_page_visits working_set_gib=%.3f zipf_theta=%.3f "
                     "trigger_window=%llu dram_ratio=%.6f configured_max=%llu dynamic_max=%llu "
                     "effective_max=%llu\n",
                     working_set_gib,
                     zipf_theta,
                     static_cast<unsigned long long>(trigger_window),
                     dram_ratio,
                     static_cast<unsigned long long>(configured),
                     static_cast<unsigned long long>(dynamic),
                     static_cast<unsigned long long>(effective));
        std::fflush(stdout);
    }
};


} // namespace leanstore::storage::two_level_admission_control