// =====================================================================
// CircularBuffer.hpp
//
// BF-Tree paper [Hao & Chandramouli, PVLDB '24] §4 — Circular buffer.
//
// This is BF-Tree's variable-length buffer pool. Unlike a conventional
// fixed-page buffer pool, the circular buffer stores variable-sized
// MiniPages and orders them in a FIFO. Eviction happens at the head;
// new mini-pages append at the tail.
//
// Paper §4.1 (Memory regions): the buffer is partitioned into three
// regions by three logical addresses (head / tail / second-chance):
//
//        head ----- 10% copy-on-access ----- second-chance ----- tail
//                                                  90% in-place-update
//
//   * 10% copy-on-access region (between head and second-chance):
//     when a mini-page in this region is accessed, it is COPIED to the
//     tail (giving it a second chance) and cold clean slots are dropped.
//     This emulates a soft LRU on top of a pure FIFO.
//   * 90% in-place-update region (between second-chance and tail):
//     mini-pages are modified in place, no copying.
//
// Paper §4.2 (Circular buffer API): get / insert / move-to-tail /
// evict-from-head primitives.
//
// Paper §4.3 (Performance optimizations): memory fragmentation, alignments
// — these are physical-allocator concerns not relevant to our integration
// (LeanStore handles physical memory; we only manage logical mini-page
// metadata mapped by page_id).
//
// Paper mapping:
//   §4   variable-length buffer pool        -> class CircularBuffer
//   §4.1 head / second-chance / tail layout -> circular_order_ list +
//                                              copy_region_pages_ set
//   §4.1 10% copy-on-access region          -> rebuildCopyRegionLocked()
//   §4.1 copy-on-access semantics           -> moveToTailWithCopyOnAccess()
//   §4.2 mini-page lookup                   -> getOrCreate() / find()
//   §4.2 head eviction                      -> iterateForEviction()
//   §3.3 mapping table (page_id -> page)    -> mini_pages_ unordered_map
//
// Concurrency: this class is a passive container; the caller (the owning
// BFTreeAdmissionPolicy) holds the master mutex around all method
// invocations. We deliberately do NOT use an internal mutex to avoid
// double-locking against the policy's hot path mutex.
// =====================================================================

#pragma once

#include "MiniPage.hpp"
#include "Units.hpp"

#include <algorithm>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace leanstore::storage::bf_tree {

class CircularBuffer {
public:
   // Paper §4.1: copy-on-access region is 10% of the buffer.
   static constexpr double kCopyOnAccessRegionFraction = 0.10;
   // Refresh the copy-region snapshot every N mutations to amortize cost
   // (we do not need exact tracking; a slightly stale region is fine).
   static constexpr u64    kCopyRegionRefreshInterval  = 256;

   // ---- Paper §4.2 API ----

   // Look up a mini-page by page_id; create a new one if absent.
   // Returns (mini-page reference, bool was_inserted).
   std::pair<MiniPage&, bool> getOrCreate(u64 page_id) {
      auto [it, inserted] = mini_pages_.try_emplace(page_id);
      if (inserted) {
         circular_order_.push_back(page_id);
         circular_index_[page_id] = std::prev(circular_order_.end());
         copy_region_dirty_ = true;
      }
      return {it->second, inserted};
   }

   // Direct lookup; returns nullptr if missing.
   MiniPage* find(u64 page_id) {
      auto it = mini_pages_.find(page_id);
      return (it == mini_pages_.end()) ? nullptr : &it->second;
   }

   // Paper §4.1 copy-on-access: relocate a mini-page from somewhere in
   // the FIFO to the tail. Cold clean slots are dropped during the copy.
   void moveToTailWithCopyOnAccess(u64 page_id) {
      auto idx_it = circular_index_.find(page_id);
      if (idx_it == circular_index_.end()) return;
      auto pg_it = mini_pages_.find(page_id);
      if (pg_it == mini_pages_.end()) return;
      pg_it->second.trimColdCachedSlotsOnCopy();
      circular_order_.splice(circular_order_.end(), circular_order_, idx_it->second);
      circular_index_[page_id] = std::prev(circular_order_.end());
      copy_region_dirty_ = true;
   }

   // Paper §4.2 + §5.5 mini-page merge case 2 ("mini-page is cold"):
   // remove a mini-page entirely. Caller is responsible for first
   // draining any pending write-back batch via WriteBackBatcher.
   void remove(u64 page_id) {
      auto idx_it = circular_index_.find(page_id);
      if (idx_it != circular_index_.end()) {
         circular_order_.erase(idx_it->second);
         circular_index_.erase(idx_it);
      }
      mini_pages_.erase(page_id);
      copy_region_pages_.erase(page_id);
      copy_region_dirty_ = true;
   }

   // Paper §4.1: is this page currently in the 10% copy-on-access region?
   bool isInCopyRegion(u64 page_id) const {
      return copy_region_pages_.find(page_id) != copy_region_pages_.end();
   }

   // Periodic recomputation of which page_ids belong to the copy-on-access
   // region. Cheap (skips work if nothing changed since the last refresh).
   void refreshCopyRegion() {
      if (!copy_region_dirty_ && copy_region_refresh_countdown_ > 0) {
         copy_region_refresh_countdown_--;
         return;
      }
      rebuildCopyRegionLocked();
   }

   // Paper §4.2 head eviction: visit the FIFO front in order. The visitor
   // returns true to evict the page (which removes it from both the
   // mapping table and the FIFO order).
   //
   // Visitor signature: bool(u64 page_id, MiniPage& page)
   template <typename Fn>
   void iterateForEviction(u64 max_pages, Fn&& visitor) {
      u64 visited = 0;
      for (auto it = circular_order_.begin();
           it != circular_order_.end() && visited < max_pages; ) {
         auto pg_it = mini_pages_.find(*it);
         if (pg_it == mini_pages_.end()) {
            // Stale FIFO entry without a mapping table entry. Drop it.
            it = circular_order_.erase(it);
            continue;
         }
         const u64 pid = pg_it->first;
         const bool should_remove = visitor(pid, pg_it->second);
         if (should_remove) {
            circular_index_.erase(pid);
            mini_pages_.erase(pg_it);
            it = circular_order_.erase(it);
            copy_region_dirty_ = true;
         } else {
            ++it;
         }
         visited++;
      }
   }

   // ---- Diagnostics ----
   size_t size()             const { return mini_pages_.size(); }
   size_t copyRegionSize()   const { return copy_region_pages_.size(); }

private:
   // Paper §3.3 mapping table: page_id -> mini-page state.
   std::unordered_map<u64, MiniPage> mini_pages_;
   // Paper §4.1 FIFO order (front = head = oldest, back = tail = newest).
   std::list<u64> circular_order_;
   // Side index for O(1) splice given a page_id.
   std::unordered_map<u64, std::list<u64>::iterator> circular_index_;
   // Paper §4.1 copy-on-access region members (front 10% of FIFO).
   std::unordered_set<u64> copy_region_pages_;
   bool copy_region_dirty_                   = true;
   u64  copy_region_refresh_countdown_       = kCopyRegionRefreshInterval;

   void rebuildCopyRegionLocked() {
      copy_region_pages_.clear();
      if (!circular_order_.empty()) {
         const size_t copy_region_len = std::max<size_t>(
             1, static_cast<size_t>(circular_order_.size() * kCopyOnAccessRegionFraction));
         size_t idx = 0;
         for (auto it = circular_order_.begin();
              it != circular_order_.end() && idx < copy_region_len;
              ++it, ++idx) {
            copy_region_pages_.insert(*it);
         }
      }
      copy_region_dirty_           = false;
      copy_region_refresh_countdown_ = kCopyRegionRefreshInterval;
   }
};

}  // namespace leanstore::storage::bf_tree
