// =====================================================================
// SecondChanceTracker.hpp
//
// HybridTier paper [Song et al., ASPLOS '25] §4 — second-chance window
// for "high-frequency, low-momentum" pages.
//
// Paper §4 motivation: a page that has been hot for a long time may
// accumulate high frequency but its access burst has faded
// (low momentum). HybridTier doesn't immediately demote such a page —
// it gives the page a "second chance": mark the page with its current
// frequency snapshot, wait one time window, then re-check. If the
// frequency has NOT risen further during the window, the page is
// genuinely cooling and gets queued for demotion. If it HAS risen,
// the page proves it's still hot and the second-chance entry is cleared.
//
// This complements the FourGridDecision::SecondChance action: that's
// the moment of MARKING, this class is the time-window state.
//
// Paper mapping:
//   §4 second chance for high-F + low-M -> Mark()
//   §4 time-window expiry + re-evaluation -> DrainExpired()
//   §4 1-minute default window           -> kSecondChanceWindow
//
// Concurrency: passive container; the owning AdmissionPolicy holds the
// master mutex around all method invocations.
// =====================================================================

#pragma once

#include "Units.hpp"
#include "leanstore/storage/buffer-manager/BufferFrame.hpp"

#include <chrono>
#include <unordered_map>
#include <utility>
#include <vector>

namespace leanstore::storage::hybried_tier_asplos2025 {

class SecondChanceTracker {
public:
   using Clock = std::chrono::steady_clock;

   // Paper §4 default second-chance window.
   static constexpr auto kSecondChanceWindow = std::chrono::minutes(1);

   struct Entry {
      leanstore::storage::BufferFrame* bf;
      u64                              marked_frequency;
      Clock::time_point                marked_at;
   };

   // Demote candidate produced by DrainExpired when a page's frequency
   // failed to rise within the second-chance window.
   struct ExpiredDemoteCandidate {
      u64                              page_id;
      leanstore::storage::BufferFrame* bf;
   };

   // Paper §4: mark a page as second-chance candidate. Records the
   // current frequency snapshot so DrainExpired can later check whether
   // the frequency rose during the window.
   void Mark(u64 page_id, leanstore::storage::BufferFrame* bf, u64 current_frequency) {
      entries_[page_id] = {bf, current_frequency, Clock::now()};
   }

   // Drop the entry (e.g., the page got promoted away or the policy
   // decided to keep it for other reasons).
   void Clear(u64 page_id) {
      entries_.erase(page_id);
   }

   bool Has(u64 page_id) const {
      return entries_.find(page_id) != entries_.end();
   }

   // Paper §4: walk all entries; for each whose window has expired,
   // re-evaluate via the user-provided `current_freq_fn(page_id) -> u64`.
   //
   //   * window not yet expired           -> keep entry, advance
   //   * expired AND frequency rose       -> page proved it's still hot,
   //                                          drop entry (no demotion)
   //   * expired AND frequency stagnant   -> push to demotion list,
   //                                          drop entry
   //
   // Returns the list of expired demote candidates. Caller emits
   // Action::DEMOTE_FULL_PAGE decisions for each one.
   template <typename CurrentFreqFn>
   std::vector<ExpiredDemoteCandidate>
   DrainExpired(Clock::time_point now, CurrentFreqFn&& current_freq_fn) {
      std::vector<ExpiredDemoteCandidate> demotes;
      demotes.reserve(entries_.size());
      for (auto it = entries_.begin(); it != entries_.end(); ) {
         if (now - it->second.marked_at < kSecondChanceWindow) {
            ++it;
            continue;
         }
         const u64 current = current_freq_fn(it->first);
         if (current <= it->second.marked_frequency) {
            // Frequency failed to rise during the window -> demote.
            demotes.push_back({it->first, it->second.bf});
         }
         it = entries_.erase(it);
      }
      return demotes;
   }

   size_t Size() const { return entries_.size(); }

private:
   std::unordered_map<u64, Entry> entries_;
};

}  // namespace leanstore::storage::hybried_tier_asplos2025
