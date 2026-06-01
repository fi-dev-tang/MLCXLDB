// =====================================================================
// WriteBackBatcher.hpp
//
// BF-Tree paper [Hao & Chandramouli, PVLDB '24] §5.4 (Delete/Update) +
// §5.5 (Mini-page operations / merge) — write-back batching.
//
// Paper §5.4 quote:
//   "The record is updated on the leaf page when the mini-page is merged."
//
// Each record write deposited into a mini-page is NOT immediately
// propagated to the underlying leaf page. Instead, the dirty slot is
// recorded in the WriteBackBatcher, and when the mini-page is merged
// (paper §5.5: either capacity-driven full-page upgrade, or FIFO
// eviction), the entire batch of dirty slots for that page is flushed
// in one shot.
//
// WHY this class exists explicitly (vs piggybacking on the dirty_slots
// bitmap inside MiniPage):
//   - It separates the "merge accounting" concern from the "per-slot
//     state" concern, matching paper's §5.4-§5.5 narrative.
//   - It exposes diagnostic counters (StashedRecords / DrainedRecords /
//     PendingPageCount) so the paper-level claim "we batch write-back
//     at merge time" is verifiable from runtime metrics.
//   - It owns its own mutex, so write-back accounting can be queried
//     by background diagnostic threads without contending the policy
//     hot-path mutex.
//
// WHAT this class does NOT do:
//   - It does not perform the actual disk write. The flush of dirty
//     records is delegated to the integration layer (BFTreeAdmissionPolicy
//     emits Action::PROMOTE_FULL_PAGE decisions; LeanStore's page
//     provider thread + WAL machinery does the durable write). This is
//     consistent with REPRODUCTION_PLAN.md §1.3 where we noted the
//     physical write path is shared with MLCXLDB.
//
// Paper mapping:
//   §5.4 deferred record update           -> StashDirtyRecord()
//   §5.5 merge-time batched write-back    -> DrainPage()
//   "merge has two triggers" (§5.5)       -> caller invokes DrainPage on
//                                            (1) full-page upgrade and
//                                            (2) FIFO head eviction
// =====================================================================

#pragma once

#include "Units.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace leanstore::storage::bf_tree {

class WriteBackBatcher {
public:
   struct DirtyRecord {
      u16 slot_id;
   };

   // Paper §5.4 Delete/Update: a record write to a mini-page is recorded
   // here as pending merge work; the leaf page is NOT touched yet.
   // Same slot written multiple times before merge is dedup'd to a
   // single batch entry — the leaf page only needs the final state.
   void StashDirtyRecord(u64 page_id, u16 slot_id) {
      std::lock_guard<std::mutex> guard(mutex_);
      auto& slots = pending_[page_id];
      for (u16 existing : slots) {
         if (existing == slot_id) return;
      }
      slots.push_back(slot_id);
      total_stashed_.fetch_add(1, std::memory_order_relaxed);
   }

   // Paper §5.5 mini-page merge: returns the entire dirty-slot batch
   // accumulated for this page and clears it. Caller is expected to
   // forward these slots to the leaf-page write path (full-page promote
   // or eviction-driven flush) within the same critical section.
   std::vector<DirtyRecord> DrainPage(u64 page_id) {
      std::vector<DirtyRecord> drained;
      std::lock_guard<std::mutex> guard(mutex_);
      auto it = pending_.find(page_id);
      if (it == pending_.end()) return drained;
      drained.reserve(it->second.size());
      for (u16 slot_id : it->second) {
         drained.push_back({slot_id});
      }
      total_drained_.fetch_add(drained.size(), std::memory_order_relaxed);
      total_merge_events_.fetch_add(1, std::memory_order_relaxed);
      pending_.erase(it);
      return drained;
   }

   bool HasPending(u64 page_id) const {
      std::lock_guard<std::mutex> guard(mutex_);
      return pending_.find(page_id) != pending_.end();
   }

   // ---- Diagnostic counters (paper §5.4-§5.5 verifiability) ----
   u64 PendingPageCount() const {
      std::lock_guard<std::mutex> guard(mutex_);
      return pending_.size();
   }
   u64 TotalStashedRecords() const { return total_stashed_.load(std::memory_order_relaxed); }
   u64 TotalDrainedRecords() const { return total_drained_.load(std::memory_order_relaxed); }
   u64 TotalMergeEvents()    const { return total_merge_events_.load(std::memory_order_relaxed); }

private:
   mutable std::mutex                              mutex_;
   std::unordered_map<u64, std::vector<u16>>       pending_;
   std::atomic<u64>                                total_stashed_       {0};
   std::atomic<u64>                                total_drained_       {0};
   std::atomic<u64>                                total_merge_events_  {0};
};

}  // namespace leanstore::storage::bf_tree
