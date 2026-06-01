#pragma once

#include "Units.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace leanstore::storage::hybried_tier_asplos2025 {

namespace cbf_detail {

inline constexpr uint32_t hash(uint64_t key) noexcept {
   const char* data = reinterpret_cast<const char*>(&key);
   uint32_t h = 0;
   for (int i = 0; i < 8; ++i) {
      h += data[i];
      h += h << 10;
      h ^= h >> 6;
   }
   h += h << 3;
   h ^= h >> 11;
   h += h << 15;
   return h;
}

inline constexpr uint32_t nearest_power_of_two(uint32_t x) noexcept {
   --x;
   x |= x >> 1;
   x |= x >> 2;
   x |= x >> 4;
   x |= x >> 8;
   x |= x >> 16;
   ++x;
   return x;
}

}  // namespace cbf_detail

// Count Bloom Filter (CBF) — faithful port from HybridTier ASPLOS'25 artifact.
// 4-bit counters packed into 64-bit blocks (16 counters per block).
// Cache-line aligned allocation. 4 hash functions per element.
// Max counter = 15. W-aging halves all counters when size_ reaches sample_size_.
class CountBloomFilter {
   uint64_t* table_       = nullptr;
   uint64_t  table_size_  = 0;
   uint64_t  size_        = 0;
   uint64_t  sample_size_ = 0;
   uint64_t  blockMask_   = 0;

   uint64_t frequency_dist_[16] = {0};

public:
   CountBloomFilter() = default;

   CountBloomFilter(uint32_t capacity, uint64_t sample_size)
       : sample_size_(sample_size)
   {
      table_size_ = cbf_detail::nearest_power_of_two(capacity);
      table_ = static_cast<uint64_t*>(aligned_alloc(64, sizeof(uint64_t) * table_size_));
      std::memset(table_, 0, sizeof(uint64_t) * table_size_);
      size_ = 0;
      blockMask_ = (table_size_ >> 3) - 1;
   }

   ~CountBloomFilter() {
      if (table_) {
         free(table_);
         table_ = nullptr;
      }
   }

   CountBloomFilter(const CountBloomFilter&) = delete;
   CountBloomFilter& operator=(const CountBloomFilter&) = delete;

   CountBloomFilter(CountBloomFilter&& o) noexcept
       : table_(o.table_), table_size_(o.table_size_), size_(o.size_),
         sample_size_(o.sample_size_), blockMask_(o.blockMask_)
   {
      std::memcpy(frequency_dist_, o.frequency_dist_, sizeof(frequency_dist_));
      o.table_ = nullptr;
      o.table_size_ = 0;
   }

   CountBloomFilter& operator=(CountBloomFilter&& o) noexcept {
      if (this != &o) {
         if (table_) free(table_);
         table_ = o.table_;
         table_size_ = o.table_size_;
         size_ = o.size_;
         sample_size_ = o.sample_size_;
         blockMask_ = o.blockMask_;
         std::memcpy(frequency_dist_, o.frequency_dist_, sizeof(frequency_dist_));
         o.table_ = nullptr;
         o.table_size_ = 0;
      }
      return *this;
   }

   uint32_t spread(uint32_t x) const {
      x ^= x >> 17;
      x *= 0xed5ad4bb;
      x ^= x >> 11;
      x *= 0xac4c1b51;
      x ^= x >> 15;
      return x;
   }

   uint32_t rehash(uint32_t x) const {
      x *= 0x31848bab;
      x ^= x >> 14;
      return x;
   }

   uint32_t frequency(uint64_t key) const noexcept {
      const uint32_t h = cbf_detail::hash(key);
      uint32_t blockHash = spread(h);
      uint32_t counterHash = rehash(blockHash);
      uint32_t block = (blockHash & blockMask_) << 3;

      uint32_t count[4];
      for (uint32_t i = 0; i < 4; i++) {
         uint32_t hi = counterHash >> (i << 3);
         uint32_t index = (hi >> 1) & 15;
         uint32_t offset = hi & 1;
         count[i] = static_cast<uint32_t>(
             (table_[block + offset + (i << 1)] >> (index << 2)) & 0xfL);
      }
      return std::min(std::min(count[0], count[1]), std::min(count[2], count[3]));
   }

   void record_access(uint64_t key, uint32_t* updated_freq) noexcept {
      const uint32_t h = cbf_detail::hash(key);
      uint32_t blockHash = spread(h);
      uint32_t counterHash = rehash(blockHash);
      uint32_t block = (blockHash & blockMask_) << 3;

      uint32_t index[8];
      uint32_t freqs[4];

      for (int i = 0; i < 4; i++) {
         uint32_t hi = counterHash >> (i << 3);
         index[i] = (hi >> 1) & 15;
         uint32_t offset = hi & 1;
         index[i + 4] = block + offset + (i << 1);
      }

      bool was_added =
            try_increment_counter_at(index[4], index[0], &freqs[0])
          | try_increment_counter_at(index[5], index[1], &freqs[1])
          | try_increment_counter_at(index[6], index[2], &freqs[2])
          | try_increment_counter_at(index[7], index[3], &freqs[3]);

      *updated_freq = std::min(std::min(freqs[0], freqs[1]), std::min(freqs[2], freqs[3]));

      if (was_added && (++size_ >= sample_size_)) {
         age();
         *updated_freq = *updated_freq / 2;
      }

      if (was_added) {
         frequency_dist_[*updated_freq]++;
         if (*updated_freq > 1) {
            frequency_dist_[*updated_freq - 1]--;
         }
      }
   }

   void increase_frequency(uint64_t key, uint32_t increase_amount, uint32_t* updated_freq) noexcept {
      const uint32_t h = cbf_detail::hash(key);
      uint32_t blockHash = spread(h);
      uint32_t counterHash = rehash(blockHash);
      uint32_t block = (blockHash & blockMask_) << 3;

      uint32_t index[8];
      uint32_t freqs[4];

      for (int i = 0; i < 4; i++) {
         uint32_t hi = counterHash >> (i << 3);
         index[i] = (hi >> 1) & 15;
         uint32_t offset = hi & 1;
         index[i + 4] = block + offset + (i << 1);
      }

      uint64_t a0 = try_increase_counter_at(index[4], index[0], increase_amount, &freqs[0]);
      uint64_t a1 = try_increase_counter_at(index[5], index[1], increase_amount, &freqs[1]);
      uint64_t a2 = try_increase_counter_at(index[6], index[2], increase_amount, &freqs[2]);
      uint64_t a3 = try_increase_counter_at(index[7], index[3], increase_amount, &freqs[3]);

      uint64_t max_increased = std::max(std::max(a0, a1), std::max(a2, a3));

      *updated_freq = std::min(std::min(freqs[0], freqs[1]), std::min(freqs[2], freqs[3]));

      size_ += increase_amount;
      if (size_ >= sample_size_) {
         age();
         *updated_freq = *updated_freq / 2;
      }

      if (max_increased != 0) {
         frequency_dist_[*updated_freq]++;
         if (*updated_freq > max_increased) {
            frequency_dist_[*updated_freq - max_increased]--;
         }
      }
   }

   void decrement_frequency(uint64_t key) noexcept {
      const uint32_t h = cbf_detail::hash(key);
      uint32_t blockHash = spread(h);
      uint32_t counterHash = rehash(blockHash);
      uint32_t block = (blockHash & blockMask_) << 3;

      uint32_t dec_index[8];
      for (int i = 0; i < 4; i++) {
         uint32_t hi = counterHash >> (i << 3);
         dec_index[i] = (hi >> 1) & 15;
         uint32_t offset = hi & 1;
         dec_index[i + 4] = block + offset + (i << 1);
      }

      decrement_counter_at(dec_index[4], dec_index[0]);
      decrement_counter_at(dec_index[5], dec_index[1]);
      decrement_counter_at(dec_index[6], dec_index[2]);
      decrement_counter_at(dec_index[7], dec_index[3]);
   }

   void age() noexcept {
      for (uint64_t i = 0; i < table_size_; i++) {
         table_[i] = (table_[i] >> 1) & 0x7777777777777777ULL;
      }
      size_ /= 2;
      for (int i = 1; i < 8; i++) {
         frequency_dist_[i] = frequency_dist_[i * 2] + frequency_dist_[i * 2 + 1];
      }
      for (int i = 8; i < 16; i++) {
         frequency_dist_[i] = 0;
      }
   }

   void reset() noexcept {
      for (uint64_t i = 0; i < table_size_; i++) {
         table_[i] = 0;
      }
      size_ = 0;
      for (int i = 0; i < 16; i++) {
         frequency_dist_[i] = 0;
      }
   }

   uint64_t get_num_hot_pages(uint32_t hot_thresh) const {
      if (hot_thresh > 15 || hot_thresh < 1) return 0;
      uint64_t sum = 0;
      for (int i = hot_thresh; i < 16; i++) {
         sum += frequency_dist_[i];
      }
      return sum;
   }

   uint32_t find_hot_thresh(uint64_t num_fast_memory_pages) const {
      uint64_t sum = 0;
      for (uint32_t i = 15; i > 1; i--) {
         sum += frequency_dist_[i];
         if (sum > num_fast_memory_pages) {
            return i;
         }
      }
      return 2;
   }

   uint64_t get_size() const { return size_; }

   uint64_t get_num_elements() const {
      uint64_t sum = 0;
      for (int i = 0; i < 16; i++) sum += frequency_dist_[i];
      return sum;
   }

   bool is_valid() const { return table_ != nullptr; }

private:
   void decrement_counter_at(uint32_t i, uint32_t j) {
      uint64_t offset = static_cast<uint64_t>(j) << 2;
      uint64_t mask = 0xfULL << offset;
      uint64_t orig_freq = (table_[i] & mask) >> offset;
      if (orig_freq > 0) {
         table_[i] -= (1ULL << offset);
      }
   }

   uint64_t try_increase_counter_at(uint32_t i, uint32_t j, uint32_t increase_amount, uint32_t* updated_freq) {
      uint64_t add = increase_amount > 15 ? 15 : increase_amount;
      uint64_t offset = static_cast<uint64_t>(j) << 2;
      uint64_t mask = 0xfULL << offset;
      uint64_t orig_freq = (table_[i] & mask) >> offset;
      if (orig_freq >= 15) {
         *updated_freq = 15;
         return 0;
      }
      uint64_t increased_amount;
      if (orig_freq + add >= 15) {
         table_[i] |= (0xfULL << offset);
         increased_amount = 15 - orig_freq;
      } else {
         table_[i] += (add << offset);
         increased_amount = increase_amount;
      }
      *updated_freq = static_cast<uint32_t>((table_[i] & mask) >> offset);
      return increased_amount;
   }

   bool try_increment_counter_at(uint32_t i, uint32_t j, uint32_t* updated_freq) {
      uint64_t offset = static_cast<uint64_t>(j) << 2;
      uint64_t mask = 0xfULL << offset;
      if ((table_[i] & mask) != mask) {
         table_[i] += (1ULL << offset);
         *updated_freq = static_cast<uint32_t>((table_[i] & mask) >> offset);
         return true;
      }
      *updated_freq = static_cast<uint32_t>((table_[i] & mask) >> offset);
      return false;
   }
};

}  // namespace leanstore::storage::hybried_tier_asplos2025
