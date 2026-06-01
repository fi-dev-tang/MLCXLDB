#pragma once

#include "leanstore/Config.hpp"
#include "leanstore/storage/two-level-admission-control/SampledVisitHistogram.hpp"
#include "leanstore/storage/two-level-admission-control/SkewRecordAdmission.hpp"
#include "leanstore/storage/buffer-manager/BufferFrame.hpp"

#include "CountBloomFilter.hpp"
#include "MomentumSketch.hpp"
#include "FourGridDecision.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace leanstore::storage::hybried_tier_asplos2025 {

// =====================================================================
// HybridTier (ASPLOS '25) admission policy, integrated into MLCXLDB.
//
// Algorithm (paper §4):
//   - CountBloomFilter tracks per-page access frequency
//   - MomentumSketch tracks short-term temporal momentum
//   - FourGridDecision combines (in_dram, high_freq, high_momentum) into
//     Promote / SecondChance / DemoteCandidate / KeepInHot / NoAction
//
// Integration boundary (matches BFTreeAdmissionPolicy):
//   This file is the thin integration layer between HybridTier's
//   algorithmic core (above) and MLCXLDB's shared execution pipeline.
//   The DramHotPageCandidates pool is reused for sharded-mutex dedup
//   ONLY — its record-grain accounting (record_cms, slot bitmap,
//   per_page_visits) is NOT fed, and CheckAndPromote (which would emit
//   record-grain PROMOTE_RECORDS) is NOT called. BackgroundRoutine
//   drains the pool as PROMOTE_FULL_PAGE only, matching the paper's
//   page-grain semantics.
//
// Page-grain-only invariant (must hold):
//   - Worker hot path may call frequency_cbf_, momentum_sketch_, and
//     hot_page_candidates_.AddCandidate. Nothing else from the shared
//     two-level infrastructure.
//   - Background drain may call hot_page_candidates_.
//     CollectAllAsFullPagePromotions + ClearAllCandidatesByEpoch.
//     Nothing else.
//   See OnRecordAccess / BackgroundRoutine for the rationale and the
//   exhaustive list of what was removed and why.
//
// Why reuse DramHotPageCandidates at all (vs a private std::map):
//   The reference artifact maintained its own promotion_candidates_ /
//   demotion_candidates_ maps with a single std::mutex. In MLCXLDB this
//   (a) raced with admission-thread startup (lazy malloc inside the
//   policy ctor collided with CPUCounters::registerThread), and (b) gave
//   high-frequency private-pool churn to BTreeVI that triggered version
//   races on YCSB-D. The sharded-pool dedup avoids both. The HybridTier
//   algorithm (4-Grid over CBF + Momentum) is preserved verbatim.
// =====================================================================

class HybridTierASPLOS2025AdmissionPolicy {
private:
   using Decision = two_level_admission_control::DramHotPageCandidates::PromotionDecision;
   using Action   = Decision::Action;
   using Grid     = FourGridDecision::Action;

   // CBF sizing from the original HybridTier artifact:
   //   NUM_FAST_MEMORY_PAGES = hot_tier_bytes / PAGE_SIZE
   //   NUM_CBF_ENTRIES = ceil(NUM_FAST_MEMORY_PAGES * r)  where r = -k/ln(1-exp(ln(p)/k)), k=4, p=0.001
   //   SAMPLE_SIZE = NUM_FAST_MEMORY_PAGES * 16 * 10
   //   Momentum CBF: capacity = NUM_CBF_ENTRIES/100, sample_size = 1100000
   static uint32_t compute_cbf_entries(uint64_t num_fast_memory_pages) {
      constexpr int k = 4;
      constexpr double p = 0.001;
      double r = -static_cast<double>(k) / std::log(1.0 - std::exp(std::log(p) / static_cast<double>(k)));
      uint64_t m = static_cast<uint64_t>(std::ceil(static_cast<double>(num_fast_memory_pages) * r));
      return static_cast<uint32_t>(std::min<uint64_t>(m, UINT32_MAX));
   }

   // HybridTier algorithmic core (paper §4).
   CountBloomFilter frequency_cbf_;
   MomentumSketch   momentum_sketch_;

   // Paper §4 hot threshold (paper default: 5). Used directly as the
   // freq_threshold gate in OnRecordAccess. The paper's own adaptive
   // threshold mechanism is not implemented here; the constant 5 is the
   // paper-faithful default and intentionally does NOT borrow our L1
   // dynamic threshold (which is a two-level innovation).
   std::atomic<uint32_t> hot_threshold_{5};

   // MLCXLDB shared infrastructure (referenced, not owned).
   two_level_admission_control::SampledVisitHistogram& page_visit_histogram_;
   two_level_admission_control::DramHotPageCandidates& hot_page_candidates_;

public:
   HybridTierASPLOS2025AdmissionPolicy(
       two_level_admission_control::SampledVisitHistogram& page_visit_histogram,
       two_level_admission_control::DramHotPageCandidates& hot_page_candidates)
       : page_visit_histogram_(page_visit_histogram),
         hot_page_candidates_(hot_page_candidates)
   {
      const double hot_gib = FLAGS_dram_buffer_pool_gib + FLAGS_dram_recordcache_gib;
      const uint64_t hot_bytes = static_cast<uint64_t>(hot_gib * 1024.0 * 1024.0 * 1024.0);
      constexpr uint64_t page_size = 4096;
      const uint64_t num_fast_memory_pages = hot_bytes / page_size;

      const uint32_t cbf_entries = compute_cbf_entries(num_fast_memory_pages);
      const uint64_t sample_size = num_fast_memory_pages * 16 * 10;

      frequency_cbf_ = CountBloomFilter(cbf_entries, sample_size);
      momentum_sketch_ = MomentumSketch(cbf_entries / 100, 1100000);
   }

   // -----------------------------------------------------------------
   // OnRecordAccess: a worker thread just touched (page_id, slot_id).
   //
   // Paper-faithful pure-page path (no record-grain anything):
   //   1. CBF + Momentum updates (paper §4).
   //   2. 4-Grid decision using hot_threshold_ (paper default 5).
   //   3. On Grid::Promote (CXL-resident page only), AddCandidate
   //      to the shared sharded pool — this is just dedup; the pool's
   //      record-grain accounting (record_cms / slot bitmap / per_page_visits)
   //      is intentionally NOT fed so BackgroundRoutine can only emit
   //      PROMOTE_FULL_PAGE.
   //
   // What was deliberately REMOVED vs the prior leaky version (and why):
   //   - page_visit_histogram_.WorkerThreadOnPageAccess: feeds OUR L1
   //     SampledVisitHistogram (two-level innovation). Skipping it keeps
   //     hybridtier from inheriting our adaptive page threshold.
   //   - hot_page_candidates_.OnPageAccess: feeds OUR global request
   //     counter used by Condition-1 dynamic ceiling. Same reason.
   //   - hot_page_candidates_.OnRecordAccess: feeds RecordCMS + the
   //     candidate's slot bitmap + per_page_visits. These are precisely
   //     the inputs to CheckAndPromote's PROMOTE_RECORDS path (Conditions
   //     0 and 3 in SkewRecordAdmission.hpp). Letting hybridtier write
   //     them and then read them back would silently grant it our
   //     record-grain promotion — the very thing this baseline must
   //     NOT have, to expose the benefit of our record-grain design.
   //   - page_visit_histogram_.GetAdmissionThreshold_fine() as
   //     freq_threshold: replaced with hot_threshold_ (paper default 5)
   //     so the high_freq gate is fully paper-internal.
   //
   // SecondChance / DemoteCandidate / KeepInHot / NoAction intentionally
   // do nothing — the demotion half of the policy is not wired in MLCXLDB.
   // -----------------------------------------------------------------
   void OnRecordAccess(u64 page_id,
                       u16 slot_id,
                       bool is_in_dram,
                       u16 page_slot_num,
                       leanstore::storage::BufferFrame* bf)
   {
      uint32_t updated_freq = 0;
      frequency_cbf_.record_access(page_id, &updated_freq);
      momentum_sketch_.Increment(page_id);

      const u64  freq_threshold = hot_threshold_.load(std::memory_order_relaxed);
      const bool high_freq      = updated_freq >= freq_threshold;
      const bool high_momentum  = momentum_sketch_.IsHigh(page_id);
      const Grid action         = FourGridDecision::Decide(is_in_dram, high_freq, high_momentum);

      if (action != Grid::Promote || is_in_dram || bf == nullptr) {
         return;
      }

      // Dedup-only AddCandidate. We deliberately do NOT call
      // hot_page_candidates_.OnRecordAccess — see the comment block above.
      hot_page_candidates_.AddCandidate(page_id, updated_freq, page_slot_num, bf, is_in_dram);

      // slot_id is intentionally unused on the page-grain path.
      (void)slot_id;
   }

   // -----------------------------------------------------------------
   // BackgroundRoutine: drain every accumulated candidate as
   // PROMOTE_FULL_PAGE and clear the pool immediately.
   //
   // Why NOT CheckAndPromote: CheckAndPromote (SkewRecordAdmission.hpp)
   // emits PROMOTE_RECORDS for skew (Conditions 0 + 3), and
   // PROMOTE_RECORDS is dispatched downstream
   // (TwoLevelAdmissionThread.cpp:38-42) to promoteRecordCacheEntry —
   // which IS our record-grain RecordCache path. Calling it from the
   // hybridtier baseline would silently grant the baseline our
   // record-grain promotion.
   //
   // CollectAllAsFullPagePromotions + ClearAllCandidatesByEpoch matches
   // the paper semantics: 4-Grid's high_freq/high_momentum check IS the
   // promotion gate, so once a page becomes a candidate it gets promoted
   // on the next drain tick. No accumulation thresholds, no skew detection.
   // -----------------------------------------------------------------
   std::vector<Decision> BackgroundRoutine()
   {
      auto decisions = hot_page_candidates_.CollectAllAsFullPagePromotions();
      hot_page_candidates_.ClearAllCandidatesByEpoch();
      return decisions;
   }
};

}  // namespace leanstore::storage::hybried_tier_asplos2025
