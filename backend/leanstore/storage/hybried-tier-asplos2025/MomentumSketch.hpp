#pragma once

#include "CountBloomFilter.hpp"
#include "Units.hpp"

#include <atomic>

namespace leanstore::storage::hybried_tier_asplos2025 {

// Momentum sketch using a real Count Bloom Filter (CBF), ported from
// HybridTier ASPLOS'25 artifact. The momentum CBF is 1/100 the size
// of the frequency CBF, with a fast aging window (sample_size = 1100000
// from the original code).
class MomentumSketch {
public:
   static constexpr uint64_t kDefaultMomentumSampleSize = 1100000;
   static constexpr u64      kDefaultMomentumThreshold  = 3;

   MomentumSketch() = default;

   explicit MomentumSketch(uint32_t cbf_capacity, uint64_t sample_size = kDefaultMomentumSampleSize)
       : cbf_(cbf_capacity, sample_size)
   {
   }

   void Increment(u64 page_id) {
      uint32_t freq;
      cbf_.record_access(page_id, &freq);
   }

   u64 GetMomentum(u64 page_id) const {
      return const_cast<CountBloomFilter&>(cbf_).frequency(page_id);
   }

   bool IsHigh(u64 page_id, u64 threshold = kDefaultMomentumThreshold) const {
      return GetMomentum(page_id) >= threshold;
   }

   void DecrementFrequency(u64 page_id) {
      cbf_.decrement_frequency(page_id);
   }

   bool is_valid() const { return cbf_.is_valid(); }

private:
   CountBloomFilter cbf_;
};

}  // namespace leanstore::storage::hybried_tier_asplos2025
