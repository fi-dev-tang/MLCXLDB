#pragma once
#include "Adapter.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/storage/tiered-indexing-zxj/TwoTreeAdapter.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace leanstore;

// Adapter that wraps TwoTreeAdapter to conform to the Adapter<Record> interface
// used by experiment binaries. Registers two B+Trees (HOT + COLD) internally.
template <class Record>
struct TwoTreeLeanStoreAdapter : Adapter<Record> {
   using TTA = leanstore::storage::tiered_indexing_zxj::TwoTreeAdapter<Record>;
   std::unique_ptr<TTA> two_tree;
   leanstore::KVInterface* hot_btree = nullptr;
   leanstore::KVInterface* cold_btree = nullptr;
   std::string name;

   TwoTreeLeanStoreAdapter() = default;

   TwoTreeLeanStoreAdapter(LeanStore& db, const std::string& base_name)
       : TwoTreeLeanStoreAdapter(db, base_name,
             FLAGS_dram_buffer_pool_gib + FLAGS_dram_recordcache_gib) {}

   TwoTreeLeanStoreAdapter(LeanStore& db, const std::string& base_name, double hot_partition_gib)
       : name(base_name)
   {
      std::string hot_name = base_name + "_HOT";
      std::string cold_name = base_name + "_COLD";

      std::fprintf(stdout, "[3T-ADAPTER-INIT] enter base=%s vi=%d recover=%d hot_gib=%.6f\n",
                   base_name.c_str(), (int)FLAGS_vi, (int)FLAGS_recover, hot_partition_gib);
      std::fflush(stdout);

      // 3T baseline is locked to BTreeVI: comparison work targets MVCC behavior,
      // and the simplified TwoTreeAdapter relies on BTreeVI's per-page guarding
      // for concurrency. FLAGS_vi is ignored here on purpose.
      if (FLAGS_recover) {
         hot_btree = &db.retrieveBTreeVI(hot_name);
         cold_btree = &db.retrieveBTreeVI(cold_name);
      } else {
         std::fprintf(stdout, "[3T-ADAPTER-INIT] before registerBTreeVI(hot=%s)\n", hot_name.c_str());
         std::fflush(stdout);
         hot_btree = &db.registerBTreeVI(hot_name, {.enable_wal = FLAGS_wal, .use_bulk_insert = false});
         std::fprintf(stdout, "[3T-ADAPTER-INIT] after registerBTreeVI(hot)\n");
         std::fflush(stdout);

         std::fprintf(stdout, "[3T-ADAPTER-INIT] before registerBTreeVI(cold=%s)\n", cold_name.c_str());
         std::fflush(stdout);
         cold_btree = &db.registerBTreeVI(cold_name, {.enable_wal = FLAGS_wal, .use_bulk_insert = false});
         std::fprintf(stdout, "[3T-ADAPTER-INIT] after registerBTreeVI(cold)\n");
         std::fflush(stdout);
      }

      std::fprintf(stdout, "[3T-ADAPTER-INIT] before make_unique<TTA>\n");
      std::fflush(stdout);
      two_tree = std::make_unique<TTA>(*hot_btree, *cold_btree, hot_partition_gib);
      std::fprintf(stdout, "[3T-ADAPTER-INIT] after make_unique<TTA>, ctor done\n");
      std::fflush(stdout);
   }

   // -------------------------------------------------------------------------
   // Adapter<Record> interface
   // -------------------------------------------------------------------------

   void lookup1(const typename Record::Key& key, const std::function<void(const Record&)>& cb) final {
      two_tree->lookup1(key, cb);
   }

   void insert(const typename Record::Key& key, const Record& record) final {
      two_tree->insert(key, record);
   }

   // Runtime insert (YCSB-D/E warmup/measure path). MUST go through the 3T
   // hot/cold logic — routing through insert_var() (cold-only) breaks 3T's
   // tiering: lookup1 finds the new key in cold, instantly migrates it back
   // to hot, and every op becomes a cross-tree shuffle. payload_length is
   // accepted for ABI parity with LeanStoreAdapter but ignored — the hot
   // tree's TaggedPayload entry is fixed-size by design.
   void runtime_insert(const typename Record::Key& key, const Record& record, u16 /*payload_length*/) {
      two_tree->insert(key, record);
   }

   void update1(const typename Record::Key& key,
                const std::function<void(Record&)>& cb,
                UpdateSameSizeInPlaceDescriptor& desc) final {
      two_tree->update1(key, cb, desc);
   }

   void scan(const typename Record::Key& key,
             const std::function<bool(const typename Record::Key&, const Record&)>& cb,
             std::function<void()> undo) final {
      two_tree->scan(key, cb, undo);
   }

   void scanDesc(const typename Record::Key& key,
                 const std::function<bool(const typename Record::Key&, const Record&)>& cb,
                 std::function<void()> undo) final {
      // 3T paper does not optimize descending scans; use ascending with reversed buffer
      // For simplicity and fairness, we merge-sort both trees in descending order.
      // This is a best-effort implementation — real 3T would be even slower here.
      u8 key_bytes[Record::maxFoldLength()];
      u16 folded_len = Record::foldKey(key_bytes, key);

      // Collect from hot tree (descending)
      std::vector<std::pair<typename Record::Key, Record>> results;
      hot_btree->scanDesc(key_bytes, folded_len,
          [&](const u8* k, u16 kl, const u8* v, u16 vl) -> bool {
             typename Record::Key rk;
             Record::unfoldKey(k, rk);
             using TP = typename TTA::TaggedPayload;
             const TP* tp = reinterpret_cast<const TP*>(v);
             if (!tp->deleted()) {
                results.push_back({rk, tp->payload});
             }
             return results.size() < 100;
          },
          []() {});

      cold_btree->scanDesc(key_bytes, folded_len,
          [&](const u8* k, u16 kl, const u8* v, u16 vl) -> bool {
             typename Record::Key rk;
             Record::unfoldKey(k, rk);
             const Record* rec = reinterpret_cast<const Record*>(v);
             results.push_back({rk, *rec});
             return results.size() < 200;
          },
          []() {});

      // Sort descending by folded key
      std::sort(results.begin(), results.end(),
          [](const auto& a, const auto& b) {
             u8 ka[Record::maxFoldLength()], kb[Record::maxFoldLength()];
             u16 ka_len = Record::foldKey(ka, a.first);
             u16 kb_len = Record::foldKey(kb, b.first);
             u16 min_len = ka_len < kb_len ? ka_len : kb_len;
             int cmp = std::memcmp(ka, kb, min_len);
             if (cmp != 0) return cmp > 0;
             return ka_len > kb_len;
          });

      for (auto& [k, v] : results) {
         if (!cb(k, v)) break;
      }
   }

   bool erase(const typename Record::Key& key) final {
      return two_tree->erase(key);
   }

   template <class Field>
   Field lookupField(const typename Record::Key& key, Field Record::*f) {
      Field local_f;
      two_tree->lookup1(key, [&](const Record& r) { local_f = r.*f; });
      return local_f;
   }

   // -------------------------------------------------------------------------
   // Additional helpers for experiment binaries
   // -------------------------------------------------------------------------

   void report() const {
      if (two_tree) two_tree->report();
   }

   // Bulk-load: initial data goes to cold tree (simulates on-disk partition).
   // Matches LeanStoreAdapter::insert_var signature used by phase1_load_data.
   void insert_var(const typename Record::Key& key, const Record& record, u16 payload_length) {
      u8 folded_key[Record::maxFoldLength()];
      u16 folded_key_len = Record::foldKey(folded_key, key);
      auto res = cold_btree->insert(folded_key, folded_key_len,
                                    (u8*)(&record), payload_length);
      ensure(res == OP_RESULT::OK || res == OP_RESULT::ABORT_TX);
      if (res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
      }
   }

   void printTreeHeight() {
      std::printf("[3T] hot height=%llu cold height=%llu\n",
                  (unsigned long long)hot_btree->getHeight(),
                  (unsigned long long)cold_btree->getHeight());
   }

   u64 count() {
      return hot_btree->countEntries() + cold_btree->countEntries();
   }
};
