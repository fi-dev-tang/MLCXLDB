#pragma once
#include "Units.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>

namespace leanstore::storage::tiered_indexing_zxj {

static constexpr int kLockWords = 65537;

enum class LockState { UNINITIALIZED, READ_LOCKED, WRITE_LOCKED, MOVED };

class OptimisticLockTable {
public:
   OptimisticLockTable() {
      for (int i = 0; i < kLockWords; ++i) {
         words_[i].store(0, std::memory_order_relaxed);
      }
   }

private:
   alignas(64) std::atomic<u64> words_[kLockWords];
   friend class OptimisticLockGuard;
};

class OptimisticLockGuard {
public:
   OptimisticLockGuard() = delete;
   OptimisticLockGuard(const OptimisticLockGuard&) = delete;
   OptimisticLockGuard& operator=(const OptimisticLockGuard&) = delete;

   OptimisticLockGuard(OptimisticLockTable* table, u64 key)
       : table_(table), word_pos_(key % kLockWords), state_(LockState::UNINITIALIZED)
   {
      version_ = table_->words_[word_pos_].load(std::memory_order_acquire);
   }

   bool read_lock() {
      version_ = table_->words_[word_pos_].load(std::memory_order_acquire);
      if (version_ % 2 == 0) {
         state_ = LockState::READ_LOCKED;
         return true;
      }
      return false;
   }

   bool write_lock() {
      version_ = table_->words_[word_pos_].load(std::memory_order_acquire);
      if (version_ % 2 == 1) return false;
      u64 v = version_;
      if (table_->words_[word_pos_].compare_exchange_strong(v, v + 1, std::memory_order_acq_rel)) {
         version_ = v + 1;
         state_ = LockState::WRITE_LOCKED;
         return true;
      }
      return false;
   }

   bool upgrade_to_write_lock() {
      assert(version_ % 2 == 0);
      u64 v = version_;
      if (table_->words_[word_pos_].compare_exchange_strong(v, v + 1, std::memory_order_acq_rel)) {
         version_ = v + 1;
         state_ = LockState::WRITE_LOCKED;
         return true;
      }
      return false;
   }

   bool validate() const {
      return table_->words_[word_pos_].load(std::memory_order_acquire) == version_;
   }

   bool more_than_one_writer_since(u64 old_version) const {
      assert(version_ > old_version);
      return version_ - old_version > 1;
   }

   u64 get_version() const { return version_; }

   ~OptimisticLockGuard() {
      if (state_ == LockState::WRITE_LOCKED) {
         table_->words_[word_pos_].fetch_add(1, std::memory_order_release);
      }
   }

private:
   OptimisticLockTable* table_;
   u32 word_pos_;
   u64 version_;
   LockState state_;
};

}  // namespace leanstore::storage::tiered_indexing_zxj
