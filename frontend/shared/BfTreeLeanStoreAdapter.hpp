#pragma once
#include "Adapter.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/storage/bf-tree/BfTreeAdapter.hpp"
#include <cassert>
#include <cstring>
#include <memory>
#include <string>

using namespace leanstore;

template <class Record>
struct BfTreeLeanStoreAdapter : Adapter<Record> {
   using BFA = leanstore::storage::bf_tree_adapter::BfTreeAdapter<Record>;
   std::unique_ptr<BFA> bf_tree;
   leanstore::KVInterface* btree = nullptr;
   std::string name;

   BfTreeLeanStoreAdapter() = default;

   // Default ctor reads only FLAGS_dram_buffer_pool_gib because, with the new
   // bf-tree dispatch in experiment_1_ycsb_full/*.cpp, the frontend transfers
   // the DRAM BP budget into mini-page and zeros FLAGS_dram_buffer_pool_gib
   // before construction. Callers in those files must instead use the explicit
   // ctor below with the captured pre-zero value (bf_tree_mini_page_gib). Do
   // NOT add FLAGS_dram_recordcache_gib here — it is owned by the two_level
   // RecordCache budget and would double-count.
   BfTreeLeanStoreAdapter(LeanStore& db, const std::string& base_name)
       : BfTreeLeanStoreAdapter(db, base_name, FLAGS_dram_buffer_pool_gib, static_cast<u16>(sizeof(Record))) {}

   BfTreeLeanStoreAdapter(LeanStore& db, const std::string& base_name, double hot_partition_gib,
                           u16 payload_size_bytes = sizeof(Record))
       : name(base_name)
   {
      if (FLAGS_vi) {
         if (FLAGS_recover) {
            btree = &db.retrieveBTreeVI(base_name);
         } else {
            btree = &db.registerBTreeVI(base_name, {.enable_wal = FLAGS_wal, .use_bulk_insert = false});
         }
      } else {
         if (FLAGS_recover) {
            btree = &db.retrieveBTreeLL(base_name);
         } else {
            btree = &db.registerBTreeLL(base_name, {.enable_wal = FLAGS_wal, .use_bulk_insert = false});
         }
      }

      bf_tree = std::make_unique<BFA>(*btree, hot_partition_gib, payload_size_bytes);
   }

   // -------------------------------------------------------------------------
   // Adapter<Record> interface
   // -------------------------------------------------------------------------

   void lookup1(const typename Record::Key& key, const std::function<void(const Record&)>& cb) final {
      bf_tree->lookup1(key, cb);
   }

   void insert(const typename Record::Key& key, const Record& record) final {
      bf_tree->insert(key, record);
   }

   // Runtime insert (YCSB-D/E warmup/measure path). MUST go through bf_tree
   // so the new row gets admitted into the hot partition normally. The
   // insert_var() override below intentionally bypasses bf_tree for bulk
   // load; calling it from the workload path would skip admission/tiering.
   void runtime_insert(const typename Record::Key& key, const Record& record, u16 /*payload_length*/) {
      bf_tree->insert(key, record);
   }

   void update1(const typename Record::Key& key,
                const std::function<void(Record&)>& cb,
                UpdateSameSizeInPlaceDescriptor& desc) final {
      bf_tree->update1(key, cb, desc);
   }

   void scan(const typename Record::Key& key,
             const std::function<bool(const typename Record::Key&, const Record&)>& cb,
             std::function<void()> undo) final {
      bf_tree->scan(key, cb, undo);
   }

   void scanDesc(const typename Record::Key& key,
                 const std::function<bool(const typename Record::Key&, const Record&)>& cb,
                 std::function<void()> undo) final {
      // Bf-Tree paper does not optimize descending scans; collect + reverse
      u8 key_bytes[Record::maxFoldLength()];
      u16 folded_len = Record::foldKey(key_bytes, key);

      std::vector<std::pair<typename Record::Key, Record>> results;
      btree->scanDesc(key_bytes, folded_len,
          [&](const u8* k, u16 kl, const u8* v, u16 vl) -> bool {
             typename Record::Key rk;
             Record::unfoldKey(k, rk);
             const Record* rec = reinterpret_cast<const Record*>(v);
             results.push_back({rk, *rec});
             return results.size() < 200;
          },
          []() {});

      for (auto& [k, v] : results) {
         if (!cb(k, v)) break;
      }
   }

   bool erase(const typename Record::Key& key) final {
      return bf_tree->erase(key);
   }

   template <class Field>
   Field lookupField(const typename Record::Key& key, Field Record::*f) {
      Field local_f;
      bf_tree->lookup1(key, [&](const Record& r) { local_f = r.*f; });
      return local_f;
   }

   // -------------------------------------------------------------------------
   // Additional helpers for experiment binaries
   // -------------------------------------------------------------------------

   void report() const {
      if (bf_tree) bf_tree->report();
   }

   void insert_var(const typename Record::Key& key, const Record& record, u16 payload_length) {
      u8 folded_key[Record::maxFoldLength()];
      u16 folded_key_len = Record::foldKey(folded_key, key);
      auto res = btree->insert(folded_key, folded_key_len,
                               (u8*)(&record), payload_length);
      ensure(res == OP_RESULT::OK || res == OP_RESULT::ABORT_TX);
      if (res == OP_RESULT::ABORT_TX) {
         cr::Worker::my().abortTX();
      }
   }

   void printTreeHeight() {
      std::printf("[BfTree] height=%llu\n",
                  (unsigned long long)btree->getHeight());
   }

   u64 count() {
      return btree->countEntries();
   }
};
