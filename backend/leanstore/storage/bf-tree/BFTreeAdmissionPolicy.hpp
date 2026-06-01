// =====================================================================
// BFTreeAdmissionPolicy.hpp
//
// BF-Tree paper [Hao & Chandramouli, PVLDB '24] integration into
// MLCXLDB's three-tier framework. The BF-Tree-specific data structures
// and algorithms live in three sibling headers:
//
//   - MiniPage.hpp           (paper §3.1)   variable-length cache page
//   - CircularBuffer.hpp     (paper §4)     FIFO + 10% copy-on-access region
//                                           = BF-Tree's variable-length buffer pool
//   - WriteBackBatcher.hpp   (paper §5.4-§5.5) merge-time batched write-back
//
// This file is the integration layer: it implements the
// MLCXLDB-side policy interface (OnRecordAccess / OnRecordWrite /
// BackgroundRoutine, dispatched from TwoLevelAdmissionControl on
// `--admission_mode=bf-tree`) and delegates the per-page state and
// FIFO management to the three components above.
//
// Why this structure (paper-defensibility argument):
//   - Each BF-Tree paper contribution maps to exactly one component file,
//     so a reviewer can point at "BF-Tree §3.1 mini-page" and find the
//     corresponding code in MiniPage.hpp.
//   - The integration boundary (this file) is deliberately thin: it
//     plugs the three components into MLCXLDB's existing
//     PageCountMinSketch / SampledVisitHistogram / DramHotPageCandidates
//     so we have a fair-comparison surface (same hardware, same workload
//     driver, same storage primitives — only the admission/eviction
//     policy logic is swapped).
// =====================================================================

#pragma once

#include "leanstore/Config.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include "leanstore/storage/two-level-admission-control/CountMinSketch.hpp"
#include "leanstore/storage/two-level-admission-control/SampledVisitHistogram.hpp"
#include "leanstore/storage/two-level-admission-control/SkewRecordAdmission.hpp"
#include "leanstore/storage/buffer-manager/BufferFrame.hpp"

#include "MiniPage.hpp"
#include "CircularBuffer.hpp"
#include "WriteBackBatcher.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace leanstore::storage {
struct BufferFrame;
}

namespace leanstore::storage::bf_tree {

class BFTreeAdmissionPolicy {
private:
   using Decision = two_level_admission_control::DramHotPageCandidates::PromotionDecision;
   using Action   = Decision::Action;

   // Paper §6.11: empirically chosen 20% promotion rate for cold→cache
   // sampling on misses. (Paper §3.1 / §5.x motivation use 1%; §6.11 picks
   // 20% as the recency/frequency trade-off default.)
   static constexpr u64 kRecordCacheSampleProbPct = 20;

   // ---- BF-Tree-specific components (one per paper section) ----
   CircularBuffer       buffer_pool_;        // paper §4
   WriteBackBatcher     writeback_batcher_;  // paper §5.4-§5.5

   // ---- Pending decisions to be flushed by BackgroundRoutine ----
   // Pages whose mini-page crossed the merge threshold and need to be
   // upgraded to a full-page mirror at the next background tick.
   std::unordered_map<u64, BufferFrame*> pending_full_page_promotions_;

   // Master mutex protecting buffer_pool_ + pending_full_page_promotions_
   // (CircularBuffer is intentionally a passive container; this mutex
   // serializes all access. WriteBackBatcher has its own internal mutex.)
   std::mutex mutex_;

   // ---- MLCXLDB shared infrastructure (referenced, not owned) ----
   two_level_admission_control::PageCountMinSketch&        page_cms_;
   two_level_admission_control::SampledVisitHistogram&     page_visit_histogram_;
   two_level_admission_control::DramHotPageCandidates&     hot_page_candidates_;

public:
   BFTreeAdmissionPolicy(
       two_level_admission_control::PageCountMinSketch&     page_cms,
       two_level_admission_control::SampledVisitHistogram&  page_visit_histogram,
       two_level_admission_control::DramHotPageCandidates&  hot_page_candidates)
       : page_cms_(page_cms),
         page_visit_histogram_(page_visit_histogram),
         hot_page_candidates_(hot_page_candidates)
   {
   }

   // -----------------------------------------------------------------
   // OnRecordAccess: a worker thread just read record `slot_id` on page
   // `page_id`. Update mini-page state and possibly queue a full-page
   // promotion if the mini-page has reached merge threshold.
   //
   // Paper mapping:
   //   §4.1 copy-on-access     -> buffer_pool_.moveToTailWithCopyOnAccess()
   //   §6.11 20% promotion     -> kRecordCacheSampleProbPct sampling check
   //   §5.5 mini-page merge    -> page.full_page_mode → pending promotion
   // -----------------------------------------------------------------
   void OnRecordAccess(u64 page_id,
                       u16 slot_id,
                       bool is_in_dram,
                       u16 page_slot_num,
                       BufferFrame* bf)
   {
      const u16 safe_page_slot_num = std::clamp<u16>(page_slot_num, 1, MiniPage::kMaxTrackedSlots);
      const u16 safe_slot_id = std::min<u16>(slot_id, safe_page_slot_num - 1);

      page_visit_histogram_.WorkerThreadOnPageAccess(page_id);
      hot_page_candidates_.OnPageAccess();

      bool should_emit_full_page_promotion = false;
      bool is_cached_hit = false;
      {
         std::lock_guard<std::mutex> guard(mutex_);
         auto [page, inserted] = buffer_pool_.getOrCreate(page_id);
         page.slot_cap = std::max<u16>(page.slot_cap, safe_page_slot_num);
         page.bf       = bf;
         page.estimated_record_bytes = std::max<u32>(
             16, static_cast<u32>(PAGE_SIZE / std::max<u16>(1, page.slot_cap)));

         buffer_pool_.refreshCopyRegion();

         // Paper §4.1: if this page sits in the front-10% copy-on-access
         // region, accessing it triggers a copy to the FIFO tail (giving
         // the page a second chance). Cold clean slots are dropped during
         // the copy; dirty slots are preserved so write-back batches
         // are not lost.
         if (buffer_pool_.isInCopyRegion(page_id)) {
            buffer_pool_.moveToTailWithCopyOnAccess(page_id);
         }

         is_cached_hit = page.isSlotCached(safe_slot_id);
         if (is_cached_hit) {
            page.markSlotAccess(safe_slot_id);
         } else if (utils::RandomGenerator::getRandU64(0, 100) < kRecordCacheSampleProbPct) {
            // Paper §6.11 default 20%: probabilistic insertion of missed records.
            page.markSlotCached(safe_slot_id);
            page.maybeGrow();
         }

         // Paper §5.5 mini-page merge case (1): mini-page grew past merge
         // threshold. Queue a full-page promotion (handled in BackgroundRoutine).
         if (page.full_page_mode && !is_in_dram && page.bf != nullptr) {
            pending_full_page_promotions_[page_id] = page.bf;
            should_emit_full_page_promotion = true;
         }
      }

      if (should_emit_full_page_promotion) {
         return;
      }

      if (is_in_dram) {
         return;
      }

      // For pages that have NOT yet reached full-page mode, fall back to
      // MLCXLDB's record-level skew detection (DramHotPageCandidates).
      // This emulates BF-Tree's mini-page behaviour: the mini-page caches
      // individual hot slots, and only when it grows large does it merge
      // into a full-page mirror.
      const u64 page_admission_threshold = std::max<u64>(
          2, page_visit_histogram_.GetAdmissionThreshold_fine());
      const u64 current_page_visit_count = page_cms_.CMSGetPageAccessCount(page_id);
      if (current_page_visit_count < page_admission_threshold) {
         return;
      }

      hot_page_candidates_.AddCandidate(
          page_id, current_page_visit_count, safe_page_slot_num, bf, is_in_dram);
      hot_page_candidates_.OnRecordAccess(page_id, safe_slot_id, bf);
      if (is_cached_hit) {
         // Cached read counts as an extra signal for skew detection.
         hot_page_candidates_.OnRecordAccess(page_id, safe_slot_id, bf);
      }
   }

   // -----------------------------------------------------------------
   // OnRecordWrite: a worker thread just wrote (insert/update) record
   // `slot_id` on page `page_id`. Paper §5.4 says the write deposits
   // into the mini-page and the leaf page is NOT touched yet — instead,
   // the dirty slot is recorded in WriteBackBatcher for batched
   // merge-time flush.
   //
   // Paper mapping:
   //   §5.4 deferred write to mini-page  -> page.markSlotDirty()
   //   §5.4 leaf page touched at merge   -> writeback_batcher_.StashDirtyRecord()
   //   §5.5 mini-page merge case (1)     -> page.maybeGrow() + full_page_mode
   // -----------------------------------------------------------------
   void OnRecordWrite(u64 page_id,
                      u16 slot_id,
                      bool is_in_dram,
                      u16 page_slot_num,
                      BufferFrame* bf)
   {
      const u16 safe_page_slot_num = std::clamp<u16>(page_slot_num, 1, MiniPage::kMaxTrackedSlots);
      const u16 safe_slot_id = std::min<u16>(slot_id, safe_page_slot_num - 1);
      {
         std::lock_guard<std::mutex> guard(mutex_);
         auto [page, inserted] = buffer_pool_.getOrCreate(page_id);
         page.slot_cap = std::max<u16>(page.slot_cap, safe_page_slot_num);
         page.estimated_record_bytes = std::max<u32>(
             16, static_cast<u32>(PAGE_SIZE / std::max<u16>(1, page.slot_cap)));
         page.bf = bf;

         page.markSlotCached(safe_slot_id);
         page.markSlotDirty (safe_slot_id);   // Paper §5.4: dirty bit per slot
         page.maybeGrow();

         if (!is_in_dram && page.full_page_mode && page.bf != nullptr) {
            pending_full_page_promotions_[page_id] = page.bf;
         }
      }
      // Paper §5.4: record the dirty slot in the write-back batch. The
      // actual leaf-page write happens at merge time (BackgroundRoutine
      // drains the batch when emitting Action::PROMOTE_FULL_PAGE).
      // WriteBackBatcher has its own mutex so we call it after releasing
      // the policy mutex_.
      writeback_batcher_.StashDirtyRecord(page_id, safe_slot_id);
   }

   // -----------------------------------------------------------------
   // BackgroundRoutine: invoked periodically by MLCXLDB's admission
   // background thread. Drains the pending full-page promotion queue
   // (paper §5.5 mini-page merge) and forwards record-level skew
   // decisions from MLCXLDB's DramHotPageCandidates.
   //
   // Paper mapping:
   //   §5.5 merge case (1) "too large"  -> drain pending_full_page_promotions_
   //   §5.5 merge time write-back batch -> writeback_batcher_.DrainPage()
   //   (record-level promotion fallback for pre-merge mini-pages)
   // -----------------------------------------------------------------
   std::vector<Decision> BackgroundRoutine(
       [[maybe_unused]] bool dram_under_pressure = false,
       [[maybe_unused]] const std::vector<std::pair<u64, BufferFrame*>>* dram_hot_pages = nullptr)
   {
      page_visit_histogram_.BackgroundThreadTryUpdate();

      std::vector<Decision> decisions;
      decisions.reserve(FLAGS_lru_background_promote_batch * 2);

      std::unordered_set<u64> emitted_pages;
      emitted_pages.reserve(FLAGS_lru_background_promote_batch * 2);

      // Pages whose write-back batch should be drained as part of merge.
      // We collect IDs here and call WriteBackBatcher::DrainPage outside
      // the master mutex (WriteBackBatcher has its own internal mutex).
      std::vector<u64> pages_to_drain;
      pages_to_drain.reserve(FLAGS_lru_background_promote_batch);

      {
         std::lock_guard<std::mutex> guard(mutex_);
         buffer_pool_.refreshCopyRegion();

         // Paper §5.5 mini-page merge case (1): mini-page too large.
         // Each entry here represents one mini-page that has been upgraded
         // to full-page mode and needs a PROMOTE_FULL_PAGE decision.
         u64 promoted = 0;
         for (auto it = pending_full_page_promotions_.begin();
              it != pending_full_page_promotions_.end() &&
              promoted < FLAGS_lru_background_promote_batch;) {
            decisions.push_back({it->first, it->second, Action::PROMOTE_FULL_PAGE, {}});
            emitted_pages.insert(it->first);
            pages_to_drain.push_back(it->first);
            ++promoted;
            it = pending_full_page_promotions_.erase(it);
         }
      }

      // Paper §5.5: at merge time, flush the entire dirty-slot batch for
      // each merging page in one shot. Counters (TotalDrainedRecords /
      // TotalMergeEvents) make this batching observable for paper claims.
      // The physical write is performed by LeanStore's page provider +
      // WAL machinery as it materializes the PROMOTE_FULL_PAGE decisions.
      for (u64 pid : pages_to_drain) {
         auto drained = writeback_batcher_.DrainPage(pid);
         (void)drained;  // batch size is captured by WriteBackBatcher counters
      }

      // ---- Record-level promotion fallback ----
      // For pages whose mini-page has not yet reached merge threshold,
      // promote individual hot records via MLCXLDB's existing skew
      // detection. This is the "record-level cache" half of BF-Tree's
      // mini-page paradigm (paper §3.1: mini-page caches individual hot
      // records, not whole pages).
      const u64 l1_fine_threshold = page_visit_histogram_.GetAdmissionThreshold_fine();
      auto skew_decisions = hot_page_candidates_.CheckAndPromote(
          l1_fine_threshold, /*record_cache_fill_ratio=*/0.0, FLAGS_admission_scan_mode);
      for (auto& decision : skew_decisions) {
         if (emitted_pages.find(decision.page_id) != emitted_pages.end()) {
            continue;
         }

         bool promote_full_page = (decision.action == Action::PROMOTE_FULL_PAGE);
         {
            std::lock_guard<std::mutex> guard(mutex_);
            MiniPage* mp = buffer_pool_.find(decision.page_id);
            if (mp != nullptr && mp->full_page_mode) {
               // Page already at merge threshold — upgrade record-level
               // promotion to full-page promotion to keep state consistent.
               promote_full_page = true;
               if (decision.cxl_bf == nullptr) {
                  decision.cxl_bf = mp->bf;
               }
            }
         }

         if (promote_full_page) {
            decisions.push_back({decision.page_id, decision.cxl_bf, Action::PROMOTE_FULL_PAGE, {}});
            // Drain any pending write-back batch for this page so the
            // merge invariant holds: every PROMOTE_FULL_PAGE decision
            // is accompanied by a write-back drain.
            auto drained = writeback_batcher_.DrainPage(decision.page_id);
            (void)drained;
         } else {
            decision.action = Action::PROMOTE_RECORDS;
            decisions.push_back(decision);
         }
         emitted_pages.insert(decision.page_id);
      }

      return decisions;
   }

   // -----------------------------------------------------------------
   // Diagnostic accessors (used by experiment scripts to verify the
   // paper claim "we batch write-back at merge time").
   // -----------------------------------------------------------------
   u64 BufferPoolSize()        const { return buffer_pool_.size(); }
   u64 WBStashedRecords()      const { return writeback_batcher_.TotalStashedRecords(); }
   u64 WBDrainedRecords()      const { return writeback_batcher_.TotalDrainedRecords(); }
   u64 WBMergeEvents()         const { return writeback_batcher_.TotalMergeEvents(); }
};

}  // namespace leanstore::storage::bf_tree
