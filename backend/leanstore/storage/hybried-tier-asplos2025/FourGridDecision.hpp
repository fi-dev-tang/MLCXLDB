// =====================================================================
// FourGridDecision.hpp
//
// HybridTier paper [Song et al., ASPLOS '25] §4 + Table 1 — promotion /
// demotion decision matrix.
//
// Paper Table 1 (paraphrased — see paper for the actual prose):
//
//   tier      |  high freq  |  low freq
//   --------- + ----------- + -----------
//   cold      |   Promote   |   (mom?) Promote / NoAction
//   hot/DRAM  |  KeepInHot  |  (mom?) DemoteCandidate / NoAction
//
//   * if a page is in the cold tier (CXL/SSD) and EITHER frequency or
//     momentum is high -> Promote to DRAM.
//   * if a page is in the hot tier (DRAM) and BOTH frequency and
//     momentum are low -> DemoteCandidate.
//   * if a page is in the hot tier with high frequency but low momentum
//     (i.e., it WAS hot but is fading) -> SecondChance (a time window
//     to confirm the cooling trend before demoting; see SecondChanceTracker).
//   * otherwise -> KeepInHot / NoAction.
//
// This file is a pure stateless decision function — it does not depend
// on any sketch, mutex, or temporal state. The intent is to make the
// paper's Table 1 directly inspectable as code: a reviewer can point
// at `Decide()` and trace each grid cell.
//
// Paper mapping:
//   Table 1 promote/demote policies -> enum class Action + Decide()
// =====================================================================

#pragma once

#include "Units.hpp"

namespace leanstore::storage::hybried_tier_asplos2025 {

class FourGridDecision {
public:
   enum class Action {
      // Page is in cold tier AND either signal is high -> promote.
      Promote,
      // Page is in hot tier AND high frequency but low momentum -> the
      // page WAS hot, may be cooling. Caller marks it in SecondChanceTracker
      // and re-evaluates after a time window.
      SecondChance,
      // Page is in hot tier AND both signals are low -> queue for demotion.
      DemoteCandidate,
      // Page is in hot tier AND at least one signal is high (steady-state
      // hot, or low-F + high-M = currently heating up) -> keep, do nothing.
      KeepInHot,
      // Page is in cold tier AND both signals are low -> defer.
      NoAction,
   };

   // Pure decision function. No state, no side effects.
   //
   // Inputs:
   //   is_in_dram     - is the page currently in the DRAM (hot) tier?
   //   high_frequency - is the long-term frequency above the freq threshold?
   //   high_momentum  - is the short-term momentum above the momentum threshold?
   //
   // Returns the action per paper Table 1.
   static constexpr Action Decide(bool is_in_dram,
                                  bool high_frequency,
                                  bool high_momentum) {
      if (!is_in_dram) {
         // Cold tier path.
         if (high_frequency || high_momentum) {
            return Action::Promote;
         }
         return Action::NoAction;
      }

      // Hot tier path.
      if (high_frequency && !high_momentum) {
         return Action::SecondChance;
      }
      if (!high_frequency && !high_momentum) {
         return Action::DemoteCandidate;
      }
      // Either both high (steady hot) or low-F + high-M (heating up).
      return Action::KeepInHot;
   }

   // Diagnostic helper: human-readable label for an action (used in logs).
   static constexpr const char* Name(Action a) {
      switch (a) {
         case Action::Promote:         return "Promote";
         case Action::SecondChance:    return "SecondChance";
         case Action::DemoteCandidate: return "DemoteCandidate";
         case Action::KeepInHot:       return "KeepInHot";
         case Action::NoAction:        return "NoAction";
      }
      return "Unknown";
   }
};

}  // namespace leanstore::storage::hybried_tier_asplos2025
