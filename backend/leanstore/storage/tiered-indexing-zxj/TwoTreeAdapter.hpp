#pragma once
#include "DistributedCounter.hpp"
#include "Units.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/KVInterface.hpp"
#include "leanstore/concurrency-recovery/Worker.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace leanstore::storage::tiered_indexing_zxj {

// Migration writes are internal data moves, not user transactions — they must
// be visible to older concurrent readers (who would otherwise fail MVCC chain
// reconstruction). INSTANTLY_VISIBLE_BULK_INSERT stamps tuple_head with the
// "committed at logical time 0" sentinel so every reader sees the migrated
// tuple regardless of their startTS.
class ScopedInstantVisibleTx {
public:
   ScopedInstantVisibleTx()
      : saved_mode_(leanstore::cr::activeTX().current_tx_mode) {
      leanstore::cr::activeTX().current_tx_mode = leanstore::TX_MODE::INSTANTLY_VISIBLE_BULK_INSERT;
   }
   ~ScopedInstantVisibleTx() {
      leanstore::cr::activeTX().current_tx_mode = saved_mode_;
   }
   ScopedInstantVisibleTx(const ScopedInstantVisibleTx&) = delete;
   ScopedInstantVisibleTx& operator=(const ScopedInstantVisibleTx&) = delete;
private:
   leanstore::TX_MODE saved_mode_;
};

// TwoTreeAdapter: 3T-style two-tree (hot + cold) baseline adapter for
// comparison against our two_level design.
//
// Faithful to the 3T paper's *performance characteristics* (record-level
// migration, clock-based eviction, batched background eviction overhead),
// but with concurrency control simplified to work with BTreeVI's MVCC:
//   - No per-record OptimisticLockTable. The original optimistic version
//     loop livelocked under BTreeVI (longer critical sections → version
//     counter churn → infinite spin).
//   - Single std::mutex + try_lock around eviction batches: at most one
//     worker evicts at a time; the rest skip and continue. BTreeVI's own
//     HybridPageGuard handles per-page concurrency on user ops.
//
// The migration overhead is preserved intact: that's exactly the soft spot
// our two_level design is meant to outperform.
template <typename Record>
class TwoTreeAdapter {
public:
   using Key = typename Record::Key;

   // bits layout: [ref_count:3 | unused:2 | deleted:1 | referenced:1 | modified:1]
   struct alignas(1) TaggedPayload {
      Record payload;
      unsigned char bits = 0;

      bool modified() const { return bits & 1u; }
      void set_modified() { bits |= 1u; }
      void clear_modified() { bits &= ~1u; }

      unsigned char get_reference_count() const { return bits >> 5u; }
      void set_reference_count(unsigned char c) { bits = (bits & 0x1Fu) | (c << 5u); }
      void bump_reference_count() {
         auto c = get_reference_count();
         set_reference_count(c >= 7 ? 7 : c + 1);
      }
      void dec_reference_count() {
         auto c = get_reference_count();
         set_reference_count(c == 0 ? 0 : c - 1);
      }

      bool referenced() const { return bits & 2u; }
      void set_referenced() { bits |= 2u; }
      void clear_referenced() { bits &= ~2u; }

      bool deleted() const { return bits & 4u; }
      void set_deleted() { bits |= 4u; }
      void clear_deleted() { bits &= ~4u; }
   };

private:
   leanstore::KVInterface& hot_btree_;
   leanstore::KVInterface& cold_btree_;

   DistributedCounter<> hot_partition_item_count_{0};
   DistributedCounter<> upward_migrations_{0};
   DistributedCounter<> downward_migrations_{0};
   DistributedCounter<> failed_upward_migrations_{0};
   DistributedCounter<> total_lookups_{0};
   DistributedCounter<> eviction_items_{0};

   alignas(64) std::atomic<u64> hot_partition_capacity_bytes_{0};
   DistributedCounter<> hot_partition_size_bytes_{0};

   alignas(64) std::mutex eviction_mutex_;
   Key clock_hand_{};

   static constexpr u16 kClockScanLimit = 300;
   static constexpr u64 kEvictionBatchKeys = 512;

   static std::mt19937_64& tl_rng() {
      static thread_local std::mt19937_64 rng{
          static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()))};
      return rng;
   }

   u64 fold_key(u8* out, const Key& key) const {
      return Record::foldKey(out, key);
   }
   void unfold_key(const u8* in, Key& key) const {
      Record::unfoldKey(in, key);
   }

   static int compare_folded(const u8* a, u16 a_len, const u8* b, u16 b_len) {
      u16 min_len = a_len < b_len ? a_len : b_len;
      int cmp = std::memcmp(a, b, min_len);
      if (cmp != 0) return cmp;
      return (a_len < b_len) ? -1 : (a_len > b_len ? 1 : 0);
   }

   bool should_migrate() {
      if (FLAGS_ti_lazy_migration_pct >= 100) return true;
      if (FLAGS_ti_lazy_migration_pct == 0) return false;
      return (tl_rng()() % 100) < (u64)FLAGS_ti_lazy_migration_pct;
   }

   bool cache_under_pressure() const {
      return static_cast<u64>(hot_partition_size_bytes_.load()) >= hot_partition_capacity_bytes_.load();
   }

public:
   TwoTreeAdapter(leanstore::KVInterface& hot_btree,
                  leanstore::KVInterface& cold_btree,
                  double hot_partition_gib)
       : hot_btree_(hot_btree), cold_btree_(cold_btree)
   {
      hot_partition_capacity_bytes_.store(
          static_cast<u64>(hot_partition_gib * 1024.0 * 1024.0 * 1024.0));
   }

   // =========================================================================
   // Eviction
   // =========================================================================
   void evict_a_bunch() {
      std::unique_lock<std::mutex> g(eviction_mutex_, std::try_to_lock);
      if (!g.owns_lock()) return;

      u8 key_bytes[Record::maxFoldLength()];
      Key start_key = clock_hand_;
      Key last_scanned_key = start_key;
      bool reached_end = true;

      std::vector<Key> evict_keys;
      std::vector<TaggedPayload> evict_payloads;
      evict_keys.reserve(kEvictionBatchKeys);
      evict_payloads.reserve(kEvictionBatchKeys);

      u16 scanned = 0;
      u16 folded_len = fold_key(key_bytes, start_key);
      hot_btree_.scanAsc(
          key_bytes, folded_len,
          [&](const u8* key, u16, const u8* value, u16) -> bool {
             Key real_key;
             unfold_key(key, real_key);
             last_scanned_key = real_key;

             TaggedPayload* tp = const_cast<TaggedPayload*>(
                 reinterpret_cast<const TaggedPayload*>(value));

             if (!tp->deleted()) {
                tp->dec_reference_count();
                if (tp->referenced()) {
                   tp->clear_referenced();
                } else {
                   evict_keys.push_back(real_key);
                   evict_payloads.push_back(*tp);
                }
             }

             ++scanned;
             if (scanned >= kClockScanLimit || evict_keys.size() >= kEvictionBatchKeys) {
                reached_end = false;
                return false;
             }
             return true;
          },
          []() {});

      clock_hand_ = reached_end ? Key{} : last_scanned_key;

      for (size_t i = 0; i < evict_keys.size(); ++i) {
         u8 kb[Record::maxFoldLength()];
         u16 kl = fold_key(kb, evict_keys[i]);

         // Both halves of the down-migration are internal data moves: keep them
         // out of the user-tx WAL/undo path by wrapping in INSTANTLY_VISIBLE.
         ScopedInstantVisibleTx _iv;
         auto ins_res = cold_btree_.insert(
             kb, kl,
             reinterpret_cast<u8*>(&evict_payloads[i].payload),
             sizeof(Record));
         if (ins_res != OP_RESULT::OK) continue;

         auto rm_res = hot_btree_.remove(kb, kl);
         if (rm_res == OP_RESULT::OK) {
            hot_partition_item_count_--;
            hot_partition_size_bytes_ -= (sizeof(Key) + sizeof(TaggedPayload));
            downward_migrations_++;
         }
      }

      eviction_items_ += evict_keys.size();
   }

   void try_eviction() {
      if (cache_under_pressure()) evict_a_bunch();
   }

   void evict_till_safe() {
      while (cache_under_pressure()) evict_a_bunch();
   }

   // =========================================================================
   // Lookup
   // =========================================================================
   void lookup1(const Key& key, const std::function<void(const Record&)>& cb) {
      try_eviction();
      ++total_lookups_;

      u8 key_bytes[Record::maxFoldLength()];
      u16 folded_len = fold_key(key_bytes, key);

      bool found_hot = false;
      auto res = hot_btree_.lookup(key_bytes, folded_len,
          [&](const u8* payload, u16) {
             TaggedPayload* tp = const_cast<TaggedPayload*>(
                 reinterpret_cast<const TaggedPayload*>(payload));
             if (!tp->deleted()) {
                tp->set_referenced();
                tp->bump_reference_count();
                cb(tp->payload);
                found_hot = true;
             }
          });
      if (res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return;
      }
      if (found_hot) return;

      Record cold_value{};
      bool found_cold = false;
      res = cold_btree_.lookup(key_bytes, folded_len,
          [&](const u8* payload, u16 payload_length) {
             // Use the slot's actual length (insert_var stored runtime
             // payload_size_bytes, not sizeof(Record)). Reading sizeof(Record)
             // from a 100B slot OOB-reads neighbouring slot data and can
             // SIGSEGV on page-boundary slots — same root cause as N5.
             std::memcpy(&cold_value, payload, payload_length);
             found_cold = true;
          });
      if (res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return;
      }
      if (!found_cold) return;

      cb(cold_value);

      if (should_migrate()) {
         TaggedPayload tp;
         tp.payload = cold_value;
         tp.set_referenced();
         tp.bump_reference_count();
         ScopedInstantVisibleTx _iv;
         auto ins_res = hot_btree_.insert(
             key_bytes, folded_len,
             reinterpret_cast<u8*>(&tp), sizeof(TaggedPayload));
         if (ins_res == OP_RESULT::OK) {
            cold_btree_.remove(key_bytes, folded_len);
            hot_partition_item_count_++;
            hot_partition_size_bytes_ += (sizeof(Key) + sizeof(TaggedPayload));
            upward_migrations_++;
         } else if (ins_res != OP_RESULT::ABORT_TX) {
            failed_upward_migrations_++;
         }
      }
   }

   // =========================================================================
   // Insert (always lands in hot tree)
   // =========================================================================
   void insert(const Key& key, const Record& record) {
      try_eviction();

      u8 key_bytes[Record::maxFoldLength()];
      u16 folded_len = fold_key(key_bytes, key);

      TaggedPayload tp;
      tp.payload = record;
      tp.set_modified();
      tp.set_referenced();

      auto res = hot_btree_.insert(
          key_bytes, folded_len,
          reinterpret_cast<u8*>(&tp), sizeof(TaggedPayload));
      if (res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return;
      }
      if (res == OP_RESULT::OK) {
         hot_partition_item_count_++;
         hot_partition_size_bytes_ += (sizeof(Key) + sizeof(TaggedPayload));
      }
   }

   // =========================================================================
   // Update (in-place if hot; else cold-update or up-migrate then update)
   // =========================================================================
   void update1(const Key& key,
                const std::function<void(Record&)>& cb,
                UpdateSameSizeInPlaceDescriptor& caller_desc) {
      try_eviction();
      ++total_lookups_;

      // Cold tree was loaded via insert_var() with runtime payload_length
      // (e.g. YCSB-F: 100B, not sizeof(Record)=608B). Hard-coding sizeof(Record)
      // in the cold lookup/update memcpys OOB-writes 508B past the slot →
      // tramples BTreeVI version chain → heap corruption (N5).
      // The caller (LeanStoreAdapter convention under vi_delta=true) sets
      // caller_desc.slots[0].length to the runtime payload size; honour it.
      const u16 cold_slot_len = caller_desc.slots[0].length;

      u8 key_bytes[Record::maxFoldLength()];
      u16 folded_len = fold_key(key_bytes, key);

      bool in_hot = false;
      auto probe = hot_btree_.lookup(key_bytes, folded_len,
          [&](const u8* payload, u16) {
             const TaggedPayload* tp = reinterpret_cast<const TaggedPayload*>(payload);
             if (!tp->deleted()) in_hot = true;
          });
      if (probe == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return;
      }

      if (in_hot) {
         u8 desc_buf[sizeof(UpdateSameSizeInPlaceDescriptor) +
                     sizeof(UpdateSameSizeInPlaceDescriptor::Slot)];
         auto* full_desc = reinterpret_cast<UpdateSameSizeInPlaceDescriptor*>(desc_buf);
         full_desc->count = 1;
         full_desc->slots[0].offset = 0;
         full_desc->slots[0].length = sizeof(TaggedPayload);

         auto res = hot_btree_.updateSameSizeInPlace(
             key_bytes, folded_len,
             [&](u8* payload, u16) {
                TaggedPayload* tp = reinterpret_cast<TaggedPayload*>(payload);
                if (!tp->deleted()) {
                   tp->set_referenced();
                   tp->set_modified();
                   cb(tp->payload);
                }
             },
             *full_desc);
         if (res == OP_RESULT::ABORT_TX) {
            cr::Worker::my().abortTX();
            return;
         }
         if (res == OP_RESULT::OK) return;
         // fallthrough on NOT_FOUND (raced with eviction)
      }

      Record cold_value{};
      bool found_cold = false;
      auto cold_res = cold_btree_.lookup(key_bytes, folded_len,
          [&](const u8* payload, u16) {
             // Read only the runtime payload bytes; reading sizeof(Record)
             // from a 100B slot OOB-reads neighbouring slot data.
             std::memcpy(&cold_value, payload, cold_slot_len);
             found_cold = true;
          });
      if (cold_res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return;
      }
      if (!found_cold) return;

      cb(cold_value);

      if (should_migrate()) {
         TaggedPayload tp;
         tp.payload = cold_value;
         tp.set_modified();
         tp.set_referenced();
         tp.bump_reference_count();

         ScopedInstantVisibleTx _iv;
         auto ins_res = hot_btree_.insert(
             key_bytes, folded_len,
             reinterpret_cast<u8*>(&tp), sizeof(TaggedPayload));
         if (ins_res == OP_RESULT::OK) {
            cold_btree_.remove(key_bytes, folded_len);
            hot_partition_item_count_++;
            hot_partition_size_bytes_ += (sizeof(Key) + sizeof(TaggedPayload));
            upward_migrations_++;
         } else if (ins_res != OP_RESULT::ABORT_TX) {
            failed_upward_migrations_++;
         }
      } else {
         u8 cold_desc_buf[sizeof(UpdateSameSizeInPlaceDescriptor) +
                          sizeof(UpdateSameSizeInPlaceDescriptor::Slot)];
         auto* cold_desc = reinterpret_cast<UpdateSameSizeInPlaceDescriptor*>(cold_desc_buf);
         cold_desc->count = 1;
         cold_desc->slots[0].offset = 0;
         cold_desc->slots[0].length = cold_slot_len;

         auto cu_res = cold_btree_.updateSameSizeInPlace(
             key_bytes, folded_len,
             [&](u8* payload, u16) {
                // Match slot size; writing sizeof(Record) here is the actual
                // OOB write that produced N5's heap corruption.
                std::memcpy(payload, &cold_value, cold_slot_len);
             },
             *cold_desc);
         if (cu_res == OP_RESULT::ABORT_TX) {
            cr::Worker::my().abortTX();
         }
      }
   }

   // =========================================================================
   // Scan (merge from both trees)
   // =========================================================================
   void scan(const Key& start_key,
             const std::function<bool(const Key&, const Record&)>& processor,
             std::function<void()> /*undo*/) {
      static constexpr size_t kScanBufSize = 8;

      u8 key_bytes[Record::maxFoldLength()];
      Key hot_keys[kScanBufSize], cold_keys[kScanBufSize];
      Record hot_payloads[kScanBufSize], cold_payloads[kScanBufSize];
      size_t hot_len = 0, cold_len = 0;
      bool hot_end = false, cold_end = false;

      auto fill_hot = [&](const Key& startk) {
         if (hot_len < kScanBufSize && !hot_end) {
            u16 fl = fold_key(key_bytes, startk);
            hot_btree_.scanAsc(key_bytes, fl,
                [&](const u8* key, u16, const u8* value, u16) -> bool {
                   Key rk;
                   unfold_key(key, rk);
                   const TaggedPayload* tp = reinterpret_cast<const TaggedPayload*>(value);
                   if (tp->deleted()) return true;
                   hot_keys[hot_len] = rk;
                   hot_payloads[hot_len] = tp->payload;
                   hot_len++;
                   return hot_len < kScanBufSize;
                },
                []() {});
            if (hot_len < kScanBufSize) hot_end = true;
         }
      };

      auto fill_cold = [&](const Key& startk) {
         if (cold_len < kScanBufSize && !cold_end) {
            u16 fl = fold_key(key_bytes, startk);
            cold_btree_.scanAsc(key_bytes, fl,
                [&](const u8* key, u16, const u8* value, u16 value_length) -> bool {
                   Key rk;
                   unfold_key(key, rk);
                   cold_keys[cold_len] = rk;
                   // Bound by actual slot size — see lookup1 cold-read for the
                   // N5 root cause (cold slot was insert_var'd at runtime
                   // payload size, not sizeof(Record)).
                   cold_payloads[cold_len] = Record{};
                   std::memcpy(&cold_payloads[cold_len], value, value_length);
                   cold_len++;
                   return cold_len < kScanBufSize;
                },
                []() {});
            if (cold_len < kScanBufSize) cold_end = true;
         }
      };

      size_t hi = 0, ci = 0;
      fill_hot(start_key);
      fill_cold(start_key);

      while (true) {
         while (hi < hot_len && ci < cold_len) {
            u8 hk_buf[Record::maxFoldLength()], ck_buf[Record::maxFoldLength()];
            u16 hk_len = fold_key(hk_buf, hot_keys[hi]);
            u16 ck_len = fold_key(ck_buf, cold_keys[ci]);
            if (compare_folded(hk_buf, hk_len, ck_buf, ck_len) <= 0) {
               if (!processor(hot_keys[hi], hot_payloads[hi])) return;
               hi++;
            } else {
               if (!processor(cold_keys[ci], cold_payloads[ci])) return;
               ci++;
            }
         }
         if (hi < hot_len && ci == cold_len && cold_end) {
            while (hi < hot_len) {
               if (!processor(hot_keys[hi], hot_payloads[hi])) return;
               hi++;
            }
         }
         if (ci < cold_len && hi == hot_len && hot_end) {
            while (ci < cold_len) {
               if (!processor(cold_keys[ci], cold_payloads[ci])) return;
               ci++;
            }
         }
         if (hi >= hot_len && !hot_end) {
            Key last_hot = hot_keys[hot_len - 1];
            hi = 0; hot_len = 0;
            fill_hot(last_hot);
         }
         if (ci >= cold_len && !cold_end) {
            Key last_cold = cold_keys[cold_len - 1];
            ci = 0; cold_len = 0;
            fill_cold(last_cold);
         }
         if (hi >= hot_len && hot_end && ci >= cold_len && cold_end) return;
      }
   }

   // =========================================================================
   // Erase
   // =========================================================================
   bool erase(const Key& key) {
      u8 key_bytes[Record::maxFoldLength()];
      u16 folded_len = fold_key(key_bytes, key);

      bool in_hot = false;
      auto probe = hot_btree_.lookup(key_bytes, folded_len, [&](const u8*, u16) { in_hot = true; });
      if (probe == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return false;
      }
      if (in_hot) {
         auto res = hot_btree_.remove(key_bytes, folded_len);
         if (res == OP_RESULT::ABORT_TX) {
            cr::Worker::my().abortTX();
            return false;
         }
         if (res == OP_RESULT::OK) {
            hot_partition_item_count_--;
            hot_partition_size_bytes_ -= (sizeof(Key) + sizeof(TaggedPayload));
            return true;
         }
      }

      bool in_cold = false;
      auto cprobe = cold_btree_.lookup(key_bytes, folded_len, [&](const u8*, u16) { in_cold = true; });
      if (cprobe == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return false;
      }
      if (!in_cold) return false;
      auto res = cold_btree_.remove(key_bytes, folded_len);
      if (res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return false;
      }
      return (res == OP_RESULT::OK);
   }

   // =========================================================================
   // Diagnostics
   // =========================================================================
   void report() const {
      std::fprintf(stdout,
                   "[3T-TwoTree] hot_items=%lld hot_bytes=%lld/%llu "
                   "up_mig=%lld down_mig=%lld failed_up=%lld evicted=%lld lookups=%lld\n",
                   (long long)hot_partition_item_count_.load(),
                   (long long)hot_partition_size_bytes_.load(),
                   (unsigned long long)hot_partition_capacity_bytes_.load(),
                   (long long)upward_migrations_.load(),
                   (long long)downward_migrations_.load(),
                   (long long)failed_upward_migrations_.load(),
                   (long long)eviction_items_.load(),
                   (long long)total_lookups_.load());
   }

   u64 getHotPartitionItemCount() const { return hot_partition_item_count_.load(); }
   u64 getUpwardMigrations() const { return upward_migrations_.load(); }
   u64 getDownwardMigrations() const { return downward_migrations_.load(); }
};

}  // namespace leanstore::storage::tiered_indexing_zxj
