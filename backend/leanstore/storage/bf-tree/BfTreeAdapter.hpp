#pragma once
// =====================================================================
// BfTreeAdapter — minimal reproduction of Bf-Tree [PVLDB '24].
//
// Reproduced:
//   §3.1 mini-page  — variable-length (64B → PAGE_SIZE), sorted, binary-search
//   §4   buffer pool — 32-way sharded FIFO, capacity-driven eviction
//   §5.1 Get        — 20% probabilistic caching on miss (§6.11 default)
//   §5.4 deferred write-back — dirty bit, batched flush at eviction
//
// NOT reproduced (acknowledged simplifications):
//   §3.4 optimistic latch coupling  — replaced by 32-way std::mutex
//   §4.1 10% copy-on-access region  — dropped (pure FIFO)
//   §5.5 mini-page → leaf-page merge — per-record updateSameSizeInPlace
//   §3.5 fence keys / prefix compression / look-ahead bytes
// =====================================================================

#include "leanstore/Config.hpp"
#include "leanstore/KVInterface.hpp"
#include "leanstore/concurrency-recovery/Worker.hpp"
#include "leanstore/storage/tiered-indexing-zxj/DistributedCounter.hpp"
#include "Units.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <list>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

namespace leanstore::storage::bf_tree_adapter {

using tiered_indexing_zxj::DistributedCounter;

template <typename Record>
class BfTreeAdapter {
public:
   using Key = typename Record::Key;

   // -------------------------------------------------------------------
   // MiniPageEntry — compact: payload stored as vector<u8> of exactly
   // payload_size_bytes_ (not sizeof(Record)).
   // -------------------------------------------------------------------
   struct MiniPageEntry {
      Key              key;
      std::vector<u8>  payload;
      bool             dirty = false;
   };

   struct MiniPage {
      std::vector<MiniPageEntry> entries;
      u32 capacity_bytes = 64;

      static constexpr u32 kMaxCapacity = 16384;

      static int compare_folded(const u8* a, u16 a_len, const u8* b, u16 b_len) {
         u16 min_len = a_len < b_len ? a_len : b_len;
         int cmp = std::memcmp(a, b, min_len);
         if (cmp != 0) return cmp;
         return (a_len < b_len) ? -1 : (a_len > b_len ? 1 : 0);
      }

      int find(const u8* folded_key, u16 folded_len) const {
         int lo = 0, hi = static_cast<int>(entries.size()) - 1;
         while (lo <= hi) {
            int mid = (lo + hi) / 2;
            u8 mid_buf[Record::maxFoldLength()];
            u16 mid_len = Record::foldKey(mid_buf, entries[mid].key);
            int cmp = compare_folded(mid_buf, mid_len, folded_key, folded_len);
            if (cmp == 0) return mid;
            if (cmp < 0) lo = mid + 1;
            else         hi = mid - 1;
         }
         return -1;
      }

      int find_insert_pos(const u8* folded_key, u16 folded_len) const {
         int lo = 0, hi = static_cast<int>(entries.size());
         while (lo < hi) {
            int mid = (lo + hi) / 2;
            u8 mid_buf[Record::maxFoldLength()];
            u16 mid_len = Record::foldKey(mid_buf, entries[mid].key);
            int cmp = compare_folded(mid_buf, mid_len, folded_key, folded_len);
            if (cmp < 0) lo = mid + 1;
            else         hi = mid;
         }
         return lo;
      }
   };

   // -------------------------------------------------------------------
   // Shard — 32-way partition. total_allocated tracks the sum of all
   // mini-pages' capacity_bytes (the budget commitment, not used bytes).
   // -------------------------------------------------------------------
   static constexpr u32 kNumShards     = 32;
   static constexpr u32 kEvictBatchCap = 64;
   static_assert((kNumShards & (kNumShards - 1)) == 0);

   struct alignas(64) Shard {
      std::mutex                                                  mutex;
      std::list<u64>                                              fifo_order;
      std::unordered_map<u64, MiniPage>                           pages;
      std::unordered_map<u64, typename std::list<u64>::iterator>  iter_map;
      u64  total_allocated = 0;
      u64  budget          = 0;

      MiniPage* find(u64 page_id) {
         auto it = pages.find(page_id);
         return (it == pages.end()) ? nullptr : &it->second;
      }

      MiniPage& getOrCreate(u64 page_id) {
         auto it = pages.find(page_id);
         if (it != pages.end()) return it->second;
         fifo_order.push_back(page_id);
         iter_map[page_id] = std::prev(fifo_order.end());
         auto& mp = pages[page_id];
         total_allocated += mp.capacity_bytes;
         return mp;
      }

      bool over_budget() const { return total_allocated > budget; }
   };

private:
   leanstore::KVInterface&         btree_;
   std::array<Shard, kNumShards>   shards_;
   u64                             logical_page_count_ = 1;
   u32                             per_entry_bytes_    = 0;
   u16                             payload_size_bytes_ = 0;

   static constexpr u32 kCacheOnMissProbPct = 20;

   DistributedCounter<> total_lookups_{0};
   DistributedCounter<> mini_page_hits_{0};
   DistributedCounter<> btree_lookups_{0};
   DistributedCounter<> eviction_count_{0};
   DistributedCounter<> merge_writeback_records_{0};
   DistributedCounter<> dirty_entries_count_{0};

   static std::mt19937_64& tl_rng() {
      static thread_local std::mt19937_64 rng{
          static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()))};
      return rng;
   }

   u64 hash_key(const Key& key) const {
      u8 buf[Record::maxFoldLength()];
      u16 len = Record::foldKey(buf, key);
      u64 h = 14695981039346656037ULL;
      for (u16 i = 0; i < len; ++i) {
         h ^= buf[i];
         h *= 1099511628211ULL;
      }
      return h;
   }

   u64 key_to_page_id(const Key& key) const { return hash_key(key) % logical_page_count_; }
   static u32 shard_idx(u64 page_id)        { return static_cast<u32>(page_id & (kNumShards - 1)); }

   u32 mini_page_used_bytes(const MiniPage& mp) const {
      return static_cast<u32>(mp.entries.size()) * per_entry_bytes_;
   }

   // Try to grow a mini-page. Returns the growth delta (0 if already at max).
   u32 try_grow(MiniPage& mp) {
      if (mp.capacity_bytes >= MiniPage::kMaxCapacity) return 0;
      u32 old_cap = mp.capacity_bytes;
      mp.capacity_bytes *= 2;
      if (mp.capacity_bytes > MiniPage::kMaxCapacity)
         mp.capacity_bytes = MiniPage::kMaxCapacity;
      return mp.capacity_bytes - old_cap;
   }

   // Returns true if insertion succeeded (new entry added or existing updated).
   // Respects capacity: if mini-page is full and at max capacity, returns false.
   bool insert_or_update(MiniPage& mp, Shard& shard,
                         const Key& key, const u8* payload_data, u16 payload_len,
                         bool mark_dirty, const u8* folded_key, u16 folded_len) {
      int idx = mp.find(folded_key, folded_len);
      if (idx >= 0) {
         std::memcpy(mp.entries[idx].payload.data(), payload_data, payload_len);
         if (mark_dirty) mp.entries[idx].dirty = true;
         return true;
      }
      // New entry — check capacity
      u32 needed = mini_page_used_bytes(mp) + per_entry_bytes_;
      while (needed > mp.capacity_bytes) {
         u32 growth = try_grow(mp);
         if (growth == 0) return false;  // at max capacity, can't fit
         shard.total_allocated += growth;
      }
      int pos = mp.find_insert_pos(folded_key, folded_len);
      MiniPageEntry entry;
      entry.key = key;
      entry.payload.resize(payload_len);
      std::memcpy(entry.payload.data(), payload_data, payload_len);
      entry.dirty = mark_dirty;
      mp.entries.insert(mp.entries.begin() + pos, std::move(entry));
      return true;
   }

   // -------------------------------------------------------------------
   // Eviction — FIFO, flush dirty entries before drop.
   // -------------------------------------------------------------------
   void maybe_evict(u32 sidx) {
      Shard& shard = shards_[sidx];
      std::lock_guard<std::mutex> guard(shard.mutex);
      if (!shard.over_budget()) return;

      u8 desc_buf[sizeof(UpdateSameSizeInPlaceDescriptor) +
                  sizeof(UpdateSameSizeInPlaceDescriptor::Slot)];
      auto* desc = reinterpret_cast<UpdateSameSizeInPlaceDescriptor*>(desc_buf);
      desc->count           = 1;
      desc->slots[0].offset = 0;

      u32 evicted = 0;
      while (shard.over_budget() &&
             evicted < kEvictBatchCap &&
             !shard.fifo_order.empty()) {
         u64 victim = shard.fifo_order.front();
         auto page_it = shard.pages.find(victim);
         if (page_it != shard.pages.end()) {
            MiniPage& mp = page_it->second;
            for (auto& e : mp.entries) {
               if (!e.dirty) continue;
               u8  key_bytes[Record::maxFoldLength()];
               u16 flen = Record::foldKey(key_bytes, e.key);
               u16 write_len = static_cast<u16>(e.payload.size());
               desc->slots[0].length = write_len;
               std::vector<u8> write_buf(e.payload);
               auto ures = btree_.updateSameSizeInPlace(
                   key_bytes, flen,
                   [&](u8* payload, u16) {
                      std::memcpy(payload, write_buf.data(), write_len);
                   },
                   *desc);
               if (ures == OP_RESULT::OK) merge_writeback_records_++;
               --dirty_entries_count_;
            }
            shard.total_allocated -= mp.capacity_bytes;
            shard.pages.erase(page_it);
         }
         shard.fifo_order.pop_front();
         shard.iter_map.erase(victim);
         eviction_count_++;
         evicted++;
      }
   }

public:
   BfTreeAdapter(leanstore::KVInterface& btree, double hot_partition_gib, u16 payload_size_bytes)
       : btree_(btree),
         per_entry_bytes_(static_cast<u32>(sizeof(Key)) + payload_size_bytes),
         payload_size_bytes_(payload_size_bytes)
   {
      const u64 total_capacity = static_cast<u64>(hot_partition_gib * 1024.0 * 1024.0 * 1024.0);
      logical_page_count_      = std::max<u64>(1, total_capacity / 16384);
      const u64 per_shard      = total_capacity / kNumShards;
      for (auto& s : shards_) s.budget = per_shard;
   }

   // Backward-compat ctor (uses sizeof(Record) as payload size)
   BfTreeAdapter(leanstore::KVInterface& btree, double hot_partition_gib)
       : BfTreeAdapter(btree, hot_partition_gib, static_cast<u16>(sizeof(Record))) {}

   // -------------------------------------------------------------------
   // Get — paper §5.1
   // -------------------------------------------------------------------
   void lookup1(const Key& key, const std::function<void(const Record&)>& cb) {
      ++total_lookups_;

      u8  key_bytes[Record::maxFoldLength()];
      u16 folded_len = Record::foldKey(key_bytes, key);
      u64 page_id    = key_to_page_id(key);
      const u32 sidx = shard_idx(page_id);
      Shard& shard   = shards_[sidx];

      {
         std::lock_guard<std::mutex> guard(shard.mutex);
         MiniPage* mp = shard.find(page_id);
         if (mp != nullptr) {
            int idx = mp->find(key_bytes, folded_len);
            if (idx >= 0) {
               Record rec{};
               std::memcpy(&rec, mp->entries[idx].payload.data(),
                           mp->entries[idx].payload.size());
               cb(rec);
               mini_page_hits_++;
               return;
            }
         }
      }

      // Mini-page miss → B+Tree
      btree_lookups_++;
      Record value{};
      u16    value_len = 0;
      bool   found     = false;
      auto   lookup_res = btree_.lookup(key_bytes, folded_len,
          [&](const u8* payload, u16 payload_length) {
             std::memcpy(&value, payload, payload_length);
             value_len = payload_length;
             found     = true;
          });
      if (lookup_res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return;
      }
      if (!found) return;

      // Paper §5.1: cache with probability
      if ((tl_rng()() % 100) < kCacheOnMissProbPct) {
         {
            std::lock_guard<std::mutex> guard(shard.mutex);
            MiniPage& mp = shard.getOrCreate(page_id);
            insert_or_update(mp, shard, key,
                             reinterpret_cast<const u8*>(&value), value_len,
                             false, key_bytes, folded_len);
         }
         maybe_evict(sidx);
      }

      cb(value);
   }

   // -------------------------------------------------------------------
   // Insert — write-through to B+Tree only. First lookup admits via §5.1.
   // -------------------------------------------------------------------
   void insert(const Key& key, const Record& record) {
      u8  key_bytes[Record::maxFoldLength()];
      u16 folded_len = Record::foldKey(key_bytes, key);

      auto res = btree_.insert(key_bytes, folded_len,
                               const_cast<u8*>(reinterpret_cast<const u8*>(&record)),
                               sizeof(Record));
      if (res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
      }
   }

   // -------------------------------------------------------------------
   // Update — deferred write. Apply cb, mark dirty.
   // -------------------------------------------------------------------
   void update1(const Key& key,
                const std::function<void(Record&)>& cb,
                [[maybe_unused]] UpdateSameSizeInPlaceDescriptor& update_descriptor) {
      ++total_lookups_;

      u8  key_bytes[Record::maxFoldLength()];
      u16 folded_len = Record::foldKey(key_bytes, key);
      u64 page_id    = key_to_page_id(key);
      const u32 sidx = shard_idx(page_id);
      Shard& shard   = shards_[sidx];

      {
         std::lock_guard<std::mutex> guard(shard.mutex);
         MiniPage* mp = shard.find(page_id);
         if (mp != nullptr) {
            int idx = mp->find(key_bytes, folded_len);
            if (idx >= 0) {
               Record rec{};
               std::memcpy(&rec, mp->entries[idx].payload.data(),
                           mp->entries[idx].payload.size());
               cb(rec);
               std::memcpy(mp->entries[idx].payload.data(), &rec,
                           mp->entries[idx].payload.size());
               bool was_dirty = mp->entries[idx].dirty;
               mp->entries[idx].dirty = true;
               if (!was_dirty) ++dirty_entries_count_;
               mini_page_hits_++;
               return;
            }
         }
      }

      // Miss — read from B+Tree, apply cb, cache as dirty
      btree_lookups_++;
      Record value{};
      u16    value_len = 0;
      bool   found     = false;
      auto   lookup_res = btree_.lookup(key_bytes, folded_len,
          [&](const u8* payload, u16 payload_length) {
             std::memcpy(&value, payload, payload_length);
             value_len = payload_length;
             found     = true;
          });
      if (lookup_res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return;
      }
      if (!found) return;

      cb(value);
      {
         std::lock_guard<std::mutex> guard(shard.mutex);
         MiniPage& mp = shard.getOrCreate(page_id);
         bool was_dirty_before = false;
         int existing_idx = mp.find(key_bytes, folded_len);
         if (existing_idx >= 0) was_dirty_before = mp.entries[existing_idx].dirty;
         bool inserted = insert_or_update(mp, shard, key,
                                          reinterpret_cast<const u8*>(&value), value_len,
                                          true, key_bytes, folded_len);
         if (inserted && !was_dirty_before) ++dirty_entries_count_;
      }
      maybe_evict(sidx);
   }

   // -------------------------------------------------------------------
   // Erase — write-through
   // -------------------------------------------------------------------
   bool erase(const Key& key) {
      u8  key_bytes[Record::maxFoldLength()];
      u16 folded_len = Record::foldKey(key_bytes, key);
      u64 page_id    = key_to_page_id(key);
      Shard& shard   = shards_[shard_idx(page_id)];

      {
         std::lock_guard<std::mutex> guard(shard.mutex);
         MiniPage* mp = shard.find(page_id);
         if (mp != nullptr) {
            int idx = mp->find(key_bytes, folded_len);
            if (idx >= 0) {
               bool was_dirty = mp->entries[idx].dirty;
               mp->entries.erase(mp->entries.begin() + idx);
               if (was_dirty) --dirty_entries_count_;
            }
         }
      }

      auto res = btree_.remove(key_bytes, folded_len);
      if (res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
         return false;
      }
      return (res == OP_RESULT::OK);
   }

   // -------------------------------------------------------------------
   // Scan — flush dirty entries, then B+Tree scanAsc
   // -------------------------------------------------------------------
   void scan(const Key& start_key,
             const std::function<bool(const Key&, const Record&)>& processor,
             [[maybe_unused]] std::function<void()> undo) {
      drain_all_dirty();

      u8  key_bytes[Record::maxFoldLength()];
      u16 folded_len = Record::foldKey(key_bytes, start_key);
      auto ret = btree_.scanAsc(key_bytes, folded_len,
          [&](const u8* k, u16, const u8* v, u16) -> bool {
             Key rk;
             Record::unfoldKey(k, rk);
             const Record* rec = reinterpret_cast<const Record*>(v);
             return processor(rk, *rec);
          },
          []() {});
      if (ret == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
      }
   }

   void drain_all_dirty() {
      if (dirty_entries_count_.load() == 0) return;

      u8 desc_buf[sizeof(UpdateSameSizeInPlaceDescriptor) +
                  sizeof(UpdateSameSizeInPlaceDescriptor::Slot)];
      auto* desc = reinterpret_cast<UpdateSameSizeInPlaceDescriptor*>(desc_buf);
      desc->count           = 1;
      desc->slots[0].offset = 0;

      for (u32 sidx = 0; sidx < kNumShards; ++sidx) {
         Shard& shard = shards_[sidx];
         std::lock_guard<std::mutex> guard(shard.mutex);
         for (auto& [pid, mp] : shard.pages) {
            for (auto& e : mp.entries) {
               if (!e.dirty) continue;
               u8  kb[Record::maxFoldLength()];
               u16 flen = Record::foldKey(kb, e.key);
               u16 write_len = static_cast<u16>(e.payload.size());
               desc->slots[0].length = write_len;
               std::vector<u8> write_buf(e.payload);
               auto ures = btree_.updateSameSizeInPlace(
                   kb, flen,
                   [&](u8* payload, u16) {
                      std::memcpy(payload, write_buf.data(), write_len);
                   },
                   *desc);
               if (ures == OP_RESULT::OK) merge_writeback_records_++;
               e.dirty = false;
               --dirty_entries_count_;
            }
         }
      }
   }

   // -------------------------------------------------------------------
   // Diagnostics
   // -------------------------------------------------------------------
   void report() const {
      u64 total_alloc  = 0;
      u64 total_budget = 0;
      size_t total_pages = 0;
      for (const auto& s : shards_) {
         total_pages  += s.pages.size();
         total_alloc  += s.total_allocated;
         total_budget += s.budget;
      }
      const u64 total = total_lookups_.load();
      const u64 hits  = mini_page_hits_.load();
      const double hr = total > 0 ? 100.0 * hits / total : 0.0;
      std::fprintf(stdout,
                   "[BfTree] mini_page_HR=%.2f%% (%lld/%lld) "
                   "btree_lookups=%lld evictions=%lld writeback_records=%lld "
                   "dirty_now=%lld buffer_pages=%zu alloc=%llu/%llu\n",
                   hr, (long long)hits, (long long)total,
                   (long long)btree_lookups_.load(),
                   (long long)eviction_count_.load(),
                   (long long)merge_writeback_records_.load(),
                   (long long)dirty_entries_count_.load(),
                   total_pages,
                   (unsigned long long)total_alloc,
                   (unsigned long long)total_budget);
   }

   u64 getMiniPageHits()           const { return mini_page_hits_.load(); }
   u64 getBTreeLookups()           const { return btree_lookups_.load(); }
   u64 getEvictionCount()          const { return eviction_count_.load(); }
   u64 getMergeWritebackRecords()  const { return merge_writeback_records_.load(); }
};

}  // namespace leanstore::storage::bf_tree_adapter
