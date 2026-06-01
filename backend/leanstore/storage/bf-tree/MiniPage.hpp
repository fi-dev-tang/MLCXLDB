// =====================================================================
// MiniPage.hpp
//
// BF-Tree paper [Hao & Chandramouli, PVLDB '24] §3.1 — mini-page.
//
// Reproduces the variable-length, in-memory cache page that mirrors a
// subset of records of an underlying leaf page. The mini-page tracks
// per-slot state with three 64-bit slot bitmaps and dynamically grows
// its capacity until it crosses the merge threshold (~half of leaf
// page) and is upgraded to a full-page mirror (paper §5.5).
//
// In our integration the underlying leaf page is a LeanStore B+tree leaf
// of size PAGE_SIZE = 16 KB (production setting, see BufferFrame.hpp:16),
// not the paper's default 4 KB. We therefore set:
//     kMaxCapacityBytes   = PAGE_SIZE       = 16 KB
//     kMergeThresholdBytes= PAGE_SIZE / 2   =  8 KB
// matching paper §5.5 "merge threshold is about half of the leaf page".
//
// Paper mapping:
//   §3.1 mini-page                   -> class MiniPage
//   §3.1 capacity range 64 B–leaf    -> kMinCapacityBytes / kMaxCapacityBytes
//   §5.5 mini-page merge (>~half)    -> kMergeThresholdBytes / full_page_mode
//   §5.5 reference flag of K/V pair  -> referenced_slots bitmap
//   §5.4 dirty record (pending merge)-> dirty_slots bitmap
//   §4.1 copy-on-access trim         -> trimColdCachedSlotsOnCopy()
//
// This class is a passive data structure. Concurrency is provided by the
// owning CircularBuffer (which holds the master mutex shared with the
// BFTreeAdmissionPolicy hot path).
// =====================================================================

#pragma once

#include "Units.hpp"
#include "leanstore/storage/buffer-manager/BufferFrame.hpp"  // for PAGE_SIZE = 16 KB

#include <algorithm>
#include <array>

namespace leanstore::storage::bf_tree {

class MiniPage {
public:
   // Paper §3.1 mini-page sizing. Adapted to LeanStore PAGE_SIZE = 16 KB.
   static constexpr u32 kMinCapacityBytes    = 64;
   static constexpr u32 kMaxCapacityBytes    = static_cast<u32>(PAGE_SIZE);          // 16 KB
   static constexpr u32 kMergeThresholdBytes = static_cast<u32>(PAGE_SIZE / 2);      //  8 KB
   // 4 × 64-bit bitmap = up to 256 slots tracked per page. With PAGE_SIZE=16 KB
   // and a typical OLTP record ≥ 64 B, 256 slots covers all realistic cases.
   static constexpr u16 kMaxTrackedSlots     = 256;

   // ---- Per-page metadata (paper §3.1) ----
   u16  slot_cap               = 1;
   u32  capacity_bytes         = kMinCapacityBytes;
   u64  used_bytes             = 0;
   u32  estimated_record_bytes = 64;
   bool full_page_mode         = false;
   leanstore::storage::BufferFrame* bf = nullptr;

   // ---- Three per-slot bitmaps ----
   std::array<u64, 4> cached_slots     = {0, 0, 0, 0};   // is the slot in mini-page (paper §3.1)
   std::array<u64, 4> referenced_slots = {0, 0, 0, 0};   // accessed since last copy/aging epoch (paper §5.5 reference flag)
   std::array<u64, 4> dirty_slots      = {0, 0, 0, 0};   // has unmerged write (paper §5.4 Delete/Update)

   // ---- Bitmap helpers ----
   static void markBit(std::array<u64, 4>& bits, u16 slot_id) {
      const u16 idx = slot_id / 64;
      const u16 bit = slot_id % 64;
      bits[idx] |= (1ULL << bit);
   }
   static bool getBit(const std::array<u64, 4>& bits, u16 slot_id) {
      const u16 idx = slot_id / 64;
      const u16 bit = slot_id % 64;
      return (bits[idx] >> bit) & 1ULL;
   }
   static void clearBit(std::array<u64, 4>& bits, u16 slot_id) {
      const u16 idx = slot_id / 64;
      const u16 bit = slot_id % 64;
      bits[idx] &= ~(1ULL << bit);
   }
   static u64 popcnt(const std::array<u64, 4>& bits) {
      return __builtin_popcountll(bits[0]) +
             __builtin_popcountll(bits[1]) +
             __builtin_popcountll(bits[2]) +
             __builtin_popcountll(bits[3]);
   }

   // ---- Per-slot state queries ----
   bool isSlotCached (u16 slot_id) const { return getBit(cached_slots,     slot_id); }
   bool isSlotDirty  (u16 slot_id) const { return getBit(dirty_slots,      slot_id); }
   u64  cachedSlotCount() const          { return popcnt(cached_slots); }
   u64  dirtySlotCount () const          { return popcnt(dirty_slots);  }

   // ---- Per-slot transitions ----
   void markSlotAccess(u16 slot_id) { markBit(referenced_slots, slot_id); }

   // Paper §3.1: insert a record into the mini-page.
   void markSlotCached(u16 slot_id) {
      if (!isSlotCached(slot_id)) {
         used_bytes += estimated_record_bytes;
      }
      markBit(cached_slots,     slot_id);
      markBit(referenced_slots, slot_id);
   }

   // Paper §5.4 Delete/Update: a record write deposits into the mini-page,
   // marking the slot dirty so the WriteBackBatcher knows there is pending
   // merge work for this page.
   void markSlotDirty (u16 slot_id) { markBit (dirty_slots, slot_id); }
   void clearSlotDirty(u16 slot_id) { clearBit(dirty_slots, slot_id); }

   // Paper §4.1 copy-on-access: when this mini-page is moved from the
   // copy-on-access region to the FIFO tail, drop "cold clean" slots
   // (cached but neither referenced nor dirty). Dirty slots are preserved
   // so that pending writes don't get dropped before merge.
   void trimColdCachedSlotsOnCopy() {
      for (u32 i = 0; i < 4; ++i) {
         const u64 cold_clean = cached_slots[i] & (~referenced_slots[i]) & (~dirty_slots[i]);
         cached_slots[i] &= ~cold_clean;
         referenced_slots[i] = 0;  // start a fresh reference epoch
      }
   }

   // Paper §3.1 / §5.5: grow capacity 2× while used > capacity, capped
   // at one leaf page. Once capacity reaches the merge threshold
   // (paper §5.5 mini-page merge case 1: "mini-page is too large"), the
   // mini-page is upgraded to full-page mirror mode.
   void maybeGrow() {
      while (!full_page_mode &&
             used_bytes >= capacity_bytes &&
             capacity_bytes < kMaxCapacityBytes) {
         capacity_bytes = std::min<u32>(capacity_bytes * 2, kMaxCapacityBytes);
      }
      if (capacity_bytes >= kMergeThresholdBytes) {
         full_page_mode = true;
         capacity_bytes = kMaxCapacityBytes;
      }
   }
};

}  // namespace leanstore::storage::bf_tree
