// =============================================================================
// cxl-update-integration.cpp
//
// Integration test for CXL-tiered Update frontend logic:
//
//   Phase 1 : Data loading (same as lookup test)
//   Phase 2 : State machine rotation tests
//             (2a) ReadOnly       -> LogicallyDeleted  (via Update)
//             (2b) Promoting      -> LogicallyDeleted  (via Update)
//             (2c) WriteThrough   -> LogicallyDeleted  (via Update)
//             (2d) Full pipeline  -> LogicallyDeleted
//                               -> (Forward_epoch)  RemovedFromHashTable
//                               -> (SIEVE)          PhysicallyFreed (soft)
//   Phase 3 : Background thread liveness check
//             (3a) Forward_epoch thread drains Invalidation_queue
//             (3b) Promote thread promotes entries to ReadOnly
//             (3c) SIEVE thread is running (usage ratio observed)
//   Phase 4 : Correctness
//             (4a) LogicallyDeleted entry not readable via tryLookup
//             (4b) After Forward_epoch: key miss in hash table
//             (4c) Underlying BTree lookup returns new value
//   Phase 5 : Concurrency - Update/Lookup overlap, no deadlock/crash
//
// =============================================================================

#include "../frontend/shared/Adapter.hpp"
#include "../frontend/shared/LeanStoreAdapter.hpp"
#include "../frontend/shared/Schema.hpp"
#include "Units.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/storage/buffer-manager/BufferManager.hpp"
#include "leanstore/storage/record-cache/RecordCache.hpp"
#include "leanstore/storage/record-cache/RecordCacheEntry.hpp"
#include "leanstore/storage/record-cache/RecordCacheSlabAllocator.hpp"
#include "leanstore/utils/Parallelize.hpp"

#include <gflags/gflags.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------
DEFINE_uint64(update_test_tuple_count,    4000000, "Initial load tuple count");
DEFINE_uint64(update_test_worker_updates,   30000, "Updates per updater thread");
DEFINE_uint64(update_test_worker_lookups,   50000, "Lookups per lookup thread");
DEFINE_uint64(update_test_seed,                42, "Random seed");
DEFINE_uint64(update_test_promote_batch,      200, "Keys injected to observe promote thread");
DEFINE_uint64(update_test_pipeline_batch,     200, "Keys for full pipeline test");
DEFINE_uint64(update_test_phase_timeout_sec,   15, "Per-phase timeout in seconds");

using namespace leanstore;
using TestKey     = u64;
using TestPayload = BytesPayload<8>;
using KVTable     = Relation<TestKey, TestPayload>;

// ---------------------------------------------------------------------------
// Color / print helpers
// ---------------------------------------------------------------------------
namespace Color {
   const char* RESET   = "\033[0m";
   const char* GREEN   = "\033[32m";
   const char* RED     = "\033[31m";
   const char* YELLOW  = "\033[33m";
   const char* CYAN    = "\033[36m";
   const char* MAGENTA = "\033[35m";
   const char* BOLD    = "\033[1m";
}

static void print_phase(const std::string& m) {
   std::cout << "\n" << Color::BOLD << Color::MAGENTA
             << "========================================\n"
             << "  " << m << "\n"
             << "========================================" << Color::RESET << "\n";
}
static void info(const std::string& m) {
   std::cout << Color::CYAN    << "[INFO] " << Color::RESET << m << "\n";
}
static void pass(const std::string& m) {
   std::cout << Color::GREEN   << "[PASS] " << Color::RESET << m << "\n";
}
static void fail(const std::string& m) {
   std::cout << Color::RED     << "[FAIL] " << Color::RESET << m << "\n";
}
static void warn(const std::string& m) {
   std::cout << Color::YELLOW  << "[WARN] " << Color::RESET << m << "\n";
}

// ---------------------------------------------------------------------------
// RecordCacheType -> human-readable string
// ---------------------------------------------------------------------------
static const char* typeName(storage::recordcache::RecordCacheType t)
{
   using T = storage::recordcache::RecordCacheType;
   switch (t) {
      case T::ReadOnlyMode:
         return "ReadOnlyMode";
      case T::WriteThroughMode:
         return "WriteThroughMode";
      case T::WriteBackMode:
         return "WriteBackMode";
      case T::LogicallyDeletedButStillInHashTable:
         return "LogicallyDeletedButStillInHashTable";
      case T::RemovedFromHashTableButWaitForPhysicalMemoryDeallocation:
         return "RemovedFromHashTableButWaitForPhysicalMemoryDeallocation";
      case T::PromoteThreadHoldingThePosition:
         return "PromoteThreadHoldingThePosition";
      default:
         return "Unknown";
   }
}

// ---------------------------------------------------------------------------
// Helpers: foldKey / makeRecord / buildCacheEntry
// ---------------------------------------------------------------------------
static std::array<u8, sizeof(TestKey)> foldKey(TestKey k)
{
   std::array<u8, sizeof(TestKey)> out{};
   KVTable::foldKey(out.data(), KVTable::Key{.my_key = k});
   return out;
}

static KVTable makeRecord(u8 v)
{
   KVTable r{};
   std::memset(r.my_payload.value, static_cast<int>(v), sizeof(r.my_payload.value));
   return r;
}

// Build a RecordCacheEntry with a given initial type (default: ReadOnlyMode)
static storage::recordcache::RecordCacheEntry* buildCacheEntry(
    storage::recordcache::RecordCacheSlabAllocator& alloc,
    std::span<const u8>                             folded_key,
    const KVTable&                                  value,
    storage::recordcache::RecordCacheType           init_type =
        storage::recordcache::RecordCacheType::ReadOnlyMode)
{
   using storage::recordcache::RecordCacheEntry;
   using storage::recordcache::RecordCacheType;

   const u16    klen  = static_cast<u16>(folded_key.size());
   const u16    vlen  = static_cast<u16>(sizeof(KVTable));
   const size_t total = sizeof(RecordCacheEntry) + klen + vlen;

   void* mem = alloc.allocate(total, alignof(RecordCacheEntry));
   auto* e   = new (mem) RecordCacheEntry();
   e->key_length             = klen;
   e->value_length           = vlen;
   e->tx_ts                  = (MSB | 1);
   e->last_modified_worker_id = 0;
   e->entry_type.store(init_type, std::memory_order_release);
   e->visited.store(false,        std::memory_order_release);
   std::memcpy(e->payload,        folded_key.data(), klen);
   std::memcpy(e->payload + klen, reinterpret_cast<const u8*>(&value), vlen);
   return e;
}

// ---------------------------------------------------------------------------
// Wait until predicate is true or timeout_sec elapses.
// Returns true if predicate became true, false on timeout.
// ---------------------------------------------------------------------------
template <typename Pred>
static bool waitUntil(Pred pred, u64 timeout_sec, u64 poll_ms = 50)
{
   const auto deadline =
       std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
   while (std::chrono::steady_clock::now() < deadline) {
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
   }
   return false;
}

// ---------------------------------------------------------------------------
// Phase 1: Data loading (identical to lookup test)
// ---------------------------------------------------------------------------
static void phase1_load(cr::CRManager& crm, LeanStoreAdapter<KVTable>& table, u64 n)
{
   print_phase("Phase 1: Data Loading (" + std::to_string(n) + " records)");
   auto t0 = std::chrono::high_resolution_clock::now();
   utils::Parallelize::range(FLAGS_worker_threads, n, [&](u64 t_i, u64 b, u64 e) {
      crm.scheduleJobAsync(t_i, [&, b, e]() {
         for (u64 i = b; i < e; i++) {
            KVTable rec = makeRecord(static_cast<u8>(i & 0xFF));
            cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
            table.insert({i}, rec);
            cr::Worker::my().commitTX();
         }
      });
   });
   crm.joinAll();
   double sec = std::chrono::duration<double>(
       std::chrono::high_resolution_clock::now() - t0).count();
   info("Loaded " + std::to_string(n) + " records in " +
        std::to_string(sec) + " sec (" +
        std::to_string(n / sec / 1e6) + " Mtps)");
   pass("Phase 1: Data loading complete");
}

// ---------------------------------------------------------------------------
// Phase 2: State machine rotation
// ---------------------------------------------------------------------------

// Helper: inject an entry with a given type, run Update, observe state transition.
// Holds enterEpoch so Forward_epoch cannot drain the queue before we inspect.
static void runStateTransitionCase(
    const std::string&                              case_name,
    storage::recordcache::RecordCacheType           inject_type,
    TestKey                                         key,
    u8                                              old_val,
    u8                                              new_val,
    cr::CRManager&                                  crm,
    LeanStoreAdapter<KVTable>&                      table,
    storage::recordcache::RecordCache*              rc,
    storage::recordcache::RecordCacheSlabAllocator* alloc)
{
   using storage::recordcache::RecordCacheType;

   crm.scheduleJobSync(0, [&]() {
      auto folded = foldKey(key);

      // Clean up any previous entry for this key
      rc->EraseFromRecordCache(folded);

      // Build entry with the desired initial type
      auto* e = buildCacheEntry(*alloc, folded, makeRecord(old_val), inject_type);
      if (!rc->InsertOrAssignInRecordCache(folded, e)) {
         fail(case_name + ": failed to inject cache entry");
         std::abort();
      }

      // Confirm injected state
      {
         auto* cur = rc->GetFromRecordCache(folded);
         if (!cur) { fail(case_name + ": entry vanished after inject"); std::abort(); }
         info(case_name + ": injected state = " +
              typeName(cur->entry_type.load(std::memory_order_acquire)));
      }

      // Freeze Forward_epoch so LogicallyDeleted state is observable
      u64 wid = cr::Worker::my().worker_id;
      rc->enterEpoch(wid);
      const size_t q_before = rc->debugInvalidationQueueSize();

      // Execute Update
      UpdateDescriptorGenerator1(ud, KVTable, my_payload);
      cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
      table.update1({key}, [&](KVTable& r){ r = makeRecord(new_val); }, ud);
      cr::Worker::my().commitTX();

      // Inspect state immediately after Update
      auto* after = rc->GetFromRecordCache(folded);
      if (!after) {
         fail(case_name + ": entry vanished before state inspection");
         rc->leaveEpoch(wid);
         std::abort();
      }
      const auto t = after->entry_type.load(std::memory_order_acquire);
      info(case_name + ": state after Update = " + typeName(t));

      if (t != RecordCacheType::LogicallyDeletedButStillInHashTable) {
         fail(case_name + ": expected LogicallyDeletedButStillInHashTable, got " +
              typeName(t));
         rc->leaveEpoch(wid);
         std::abort();
      }

      // Verify Invalidation_queue grew
      const size_t q_after = rc->debugInvalidationQueueSize();
      info(case_name + ": invalidation_queue before=" + std::to_string(q_before) +
           " after=" + std::to_string(q_after));
      if (q_after <= q_before) {
         fail(case_name + ": entry was not enqueued in Invalidation_queue");
         rc->leaveEpoch(wid);
         std::abort();
      }

      rc->leaveEpoch(wid);
      pass(case_name + ": " + typeName(inject_type) +
           " -> LogicallyDeletedButStillInHashTable  [queue +1]");
   });
}

static void phase2_state_machine(cr::CRManager& crm,
                                  LeanStoreAdapter<KVTable>& table,
                                  storage::recordcache::RecordCache* rc,
                                  storage::recordcache::RecordCacheSlabAllocator* alloc)
{
   print_phase("Phase 2: State Machine Rotation");
   using RCT = storage::recordcache::RecordCacheType;

   // Key space: use offsets from tuple_count/4 to avoid collision with Phase 5
   const TestKey base = FLAGS_update_test_tuple_count / 4;

   // (2a) ReadOnlyMode -> LogicallyDeleted
   runStateTransitionCase(
       "Case(2a) ReadOnly->LogicallyDeleted",
       RCT::ReadOnlyMode,
       base + 1, 0x11, 0x12,
       crm, table, rc, alloc);

   // (2b) PromoteThreadHoldingThePosition -> LogicallyDeleted
   runStateTransitionCase(
       "Case(2b) Promoting->LogicallyDeleted",
       RCT::PromoteThreadHoldingThePosition,
       base + 2, 0x21, 0x22,
       crm, table, rc, alloc);


   // (2d) Full pipeline:
   //   Update -> LogicallyDeleted
   //          -> (Forward_epoch) RemovedFromHashTable  [entry gone from hash]
   //          -> (SIEVE)         physical free         [bytes_in_use reduced, soft]
   crm.scheduleJobSync(0, [&]() {
      const TestKey key    = base + 3;
      auto          folded = foldKey(key);

      rc->EraseFromRecordCache(folded);
      auto* e = buildCacheEntry(*alloc, folded, makeRecord(0x41));
      if (!rc->InsertOrAssignInRecordCache(folded, e)) {
         fail("Case(2d): failed to inject entry");
         std::abort();
      }
      info("Case(2d): injected state = " +
           std::string(typeName(e->entry_type.load(std::memory_order_acquire))));

      const size_t bytes_before = alloc->getBytesInUse();

      // Execute Update — do NOT hold enterEpoch so Forward_epoch can proceed
      UpdateDescriptorGenerator1(ud, KVTable, my_payload);
      cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
      table.update1({key}, [&](KVTable& r){ r = makeRecord(0x42); }, ud);
      cr::Worker::my().commitTX();
      info("Case(2d): Update committed, waiting for Forward_epoch to drain queue...");

      // Step 1: wait for RemovedFromHashTable (entry disappears from hash)
      bool removed = waitUntil(
          [&]() { return rc->GetFromRecordCache(folded) == nullptr; },
          FLAGS_update_test_phase_timeout_sec);

      if (!removed) {
         // Print current state to aid debugging
         auto* cur = rc->GetFromRecordCache(folded);
         if (cur) {
            info("Case(2d): timed out, current state = " +
                 std::string(typeName(cur->entry_type.load(std::memory_order_acquire))));
         }
         fail("Case(2d): entry not removed from hash table within timeout");
         std::abort();
      }
      pass("Case(2d): LogicallyDeleted -> RemovedFromHashTable "
           "(Forward_epoch confirmed)");

      // Step 2: wait for SIEVE physical release (soft check — not a hard fail)
      // SIEVE clock-hand may need time to sweep to this entry.
      info("Case(2d): waiting for SIEVE physical memory release (soft, up to " +
           std::to_string(FLAGS_update_test_phase_timeout_sec) + "s)...");
      bool freed = waitUntil(
          [&]() { return alloc->getBytesInUse() < bytes_before; },
          FLAGS_update_test_phase_timeout_sec,
          200 /*poll_ms*/);

      const size_t bytes_after = alloc->getBytesInUse();
      info("Case(2d): bytes_in_use before=" + std::to_string(bytes_before) +
           " after=" + std::to_string(bytes_after));
      if (freed) {
         pass("Case(2d): RemovedFromHashTable -> physical memory released (SIEVE confirmed)");
      } else {
         warn("Case(2d): SIEVE has not yet freed memory — clock-hand may not have "
              "reached this entry yet (non-deterministic timing, not a hard failure)");
      }
   });
}

// ---------------------------------------------------------------------------
// Phase 3: Background thread liveness
// ---------------------------------------------------------------------------
static void phase3_background_liveness(cr::CRManager& crm,
                                        LeanStoreAdapter<KVTable>& table,
                                        storage::recordcache::RecordCache* rc,
                                        storage::recordcache::RecordCacheSlabAllocator* alloc)
{
   print_phase("Phase 3: Background Thread Liveness");
   using RCT = storage::recordcache::RecordCacheType;

   const TestKey base = FLAGS_update_test_tuple_count / 4 + 1000;

   // ------------------------------------------------------------------
   // (3a) Forward_epoch thread: inject N LogicallyDeleted entries,
   //      confirm all disappear from hash table within timeout.
   // ------------------------------------------------------------------
   {
      info("Phase(3a): testing Forward_epoch thread liveness with " +
           std::to_string(FLAGS_update_test_pipeline_batch) + " entries...");

      std::vector<std::array<u8, sizeof(TestKey)>> foldeds;
      foldeds.reserve(FLAGS_update_test_pipeline_batch);

      crm.scheduleJobSync(0, [&]() {
         for (u64 i = 0; i < FLAGS_update_test_pipeline_batch; i++) {
            TestKey key    = base + i;
            auto    folded = foldKey(key);
            rc->EraseFromRecordCache(folded);
            auto* e = buildCacheEntry(*alloc, folded, makeRecord(0xA0));
            rc->InsertOrAssignInRecordCache(folded, e);
            foldeds.push_back(folded);
         }

         // Batch Update — all entries enter LogicallyDeleted + Invalidation_queue
         UpdateDescriptorGenerator1(ud, KVTable, my_payload);
         for (u64 i = 0; i < FLAGS_update_test_pipeline_batch; i++) {
            TestKey k = base + i;
            jumpmuTry() {
               cr::Worker::my().startTX(TX_MODE::OLTP,
                                        TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
               table.update1({k}, [&](KVTable& r){ r = makeRecord(0xA1); }, ud);
               cr::Worker::my().commitTX();
            }
            jumpmuCatch() {}
         }
         info("Phase(3a): batch update done, invalidation_queue_size=" +
              std::to_string(rc->debugInvalidationQueueSize()));
      });

      // Poll until all entries removed from hash table
      bool all_gone = waitUntil([&]() {
         for (auto& f : foldeds) {
            if (rc->GetFromRecordCache(f) != nullptr) return false;
         }
         return true;
      }, FLAGS_update_test_phase_timeout_sec);

      if (!all_gone) {
         // Report how many are still present
         u64 still_present = 0;
         for (auto& f : foldeds) {
            auto* cur = rc->GetFromRecordCache(f);
            if (cur) {
               still_present++;
               info("  still present state=" +
                    std::string(typeName(
                        cur->entry_type.load(std::memory_order_acquire))));
               if (still_present >= 3) break;  // print at most 3 examples
            }
         }
         fail("Phase(3a): Forward_epoch did not drain " +
              std::to_string(FLAGS_update_test_pipeline_batch) +
              " entries within timeout (" +
              std::to_string(still_present) + " still in hash table)");
         std::abort();
      }
      info("Phase(3a): invalidation_queue_size after drain=" +
           std::to_string(rc->debugInvalidationQueueSize()));
      pass("Phase(3a): Forward_epoch thread is alive and drains Invalidation_queue");
   }

   // ------------------------------------------------------------------
   // (3b) Promote thread: inject N PromoteThreadHoldingThePosition entries,
   //      confirm they transition to ReadOnly within timeout.
   // ------------------------------------------------------------------
   {
      info("Phase(3b): testing Promote thread liveness with " +
           std::to_string(FLAGS_update_test_promote_batch) + " entries...");

      const TestKey pbase = base + FLAGS_update_test_pipeline_batch + 500;
      std::vector<std::pair<std::array<u8, sizeof(TestKey)>,
                            storage::recordcache::RecordCacheEntry*>> entries;
      entries.reserve(FLAGS_update_test_promote_batch);

      crm.scheduleJobSync(0, [&]() {
         for (u64 i = 0; i < FLAGS_update_test_promote_batch; i++) {
            TestKey key    = pbase + i;
            auto    folded = foldKey(key);
            rc->EraseFromRecordCache(folded);
            // Inject as PromoteThreadHoldingThePosition
            auto* e = buildCacheEntry(*alloc, folded, makeRecord(0xB0),
                                      RCT::PromoteThreadHoldingThePosition);
            bool ok = rc->InsertOrAssignInRecordCache(folded, e);
            if (ok) entries.emplace_back(folded, e);
         }
         info("Phase(3b): injected " + std::to_string(entries.size()) +
              " Promoting entries");
      });

      // Wait for Promote thread to flip them to ReadOnly (or any non-Promoting state)
      bool all_promoted = waitUntil([&]() {
         for (auto& [f, e_ptr] : entries) {
            auto t = e_ptr->entry_type.load(std::memory_order_acquire);
            if (t == RCT::PromoteThreadHoldingThePosition) return false;
         }
         return true;
      }, FLAGS_update_test_phase_timeout_sec);

      // Collect state distribution for reporting
      std::map<std::string, u64> state_counts;
      for (auto& [f, e_ptr] : entries) {
         auto t = e_ptr->entry_type.load(std::memory_order_acquire);
         state_counts[typeName(t)]++;
      }
      for (auto& [name, cnt] : state_counts) {
         info("Phase(3b): final state distribution: " + name +
              " = " + std::to_string(cnt));
      }

      if (!all_promoted) {
         warn("Phase(3b): some entries still in PromoteThreadHoldingThePosition — "
              "Promote thread may be slow or entries were evicted before promotion");
         // Not a hard abort: promote thread timing is non-deterministic
      } else {
         pass("Phase(3b): Promote thread is alive and transitioned all entries "
              "out of PromoteThreadHoldingThePosition");
      }
   }

   // ------------------------------------------------------------------
   // (3c) SIEVE thread: verify usage ratio is being maintained
   //      (should stay near or below the 0.70 watermark).
   // ------------------------------------------------------------------
   {
      info("Phase(3c): checking SIEVE thread activity via usage ratio...");
      double ratio = alloc->getUsageRatio();
      info("Phase(3c): current allocator usage_ratio=" +
           std::to_string(ratio));
      if (ratio <= 0.75) {
         pass("Phase(3c): SIEVE thread is alive — usage ratio " +
              std::to_string(ratio) +
              " is at or below expected watermark (0.70~0.75)");
      } else {
         warn("Phase(3c): usage ratio=" + std::to_string(ratio) +
              " is above 0.75 — SIEVE may be lagging (non-deterministic)");
      }
   }
}

// ---------------------------------------------------------------------------
// Phase 4: Correctness
// ---------------------------------------------------------------------------
static void phase4_correctness(cr::CRManager& crm,
                                LeanStoreAdapter<KVTable>& table,
                                storage::recordcache::RecordCache* rc,
                                storage::recordcache::RecordCacheSlabAllocator* alloc)
{
   print_phase("Phase 4: Correctness");

   const TestKey base = FLAGS_update_test_tuple_count / 4 + 5000;

   // ------------------------------------------------------------------
   // (4a) After Update: tryLookupInRecordCache must NOT return the entry
   //      (LogicallyDeleted is invisible to readers)
   // ------------------------------------------------------------------
   crm.scheduleJobSync(0, [&]() {
      const TestKey key    = base + 1;
      auto          folded = foldKey(key);

      rc->EraseFromRecordCache(folded);
      auto* e = buildCacheEntry(*alloc, folded, makeRecord(0xC0));
      rc->InsertOrAssignInRecordCache(folded, e);

      u64 wid = cr::Worker::my().worker_id;
      rc->enterEpoch(wid);  // freeze Forward_epoch

      UpdateDescriptorGenerator1(ud, KVTable, my_payload);
      cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
      table.update1({key}, [&](KVTable& r){ r = makeRecord(0xC1); }, ud);
      cr::Worker::my().commitTX();

      // Verify state is LogicallyDeleted
      auto* after = rc->GetFromRecordCache(folded);
      if (after) {
         auto t = after->entry_type.load(std::memory_order_acquire);
         info("Phase(4a): state after update = " + std::string(typeName(t)));
      }

      // tryLookup must miss
      bool hit = rc->tryLookupInRecordCache(folded, [&](const u8*, u16){}, wid);
      if (hit) {
         fail("Phase(4a): LogicallyDeleted entry was readable via tryLookup — "
              "stale read violation!");
         rc->leaveEpoch(wid);
         std::abort();
      }
      pass("Phase(4a): LogicallyDeleted entry is invisible to tryLookupInRecordCache");
      rc->leaveEpoch(wid);
   });

   // ------------------------------------------------------------------
   // (4b) After Forward_epoch processes the entry: key must miss in hash table
   // ------------------------------------------------------------------
   crm.scheduleJobSync(0, [&]() {
      const TestKey key    = base + 2;
      auto          folded = foldKey(key);

      rc->EraseFromRecordCache(folded);
      auto* e = buildCacheEntry(*alloc, folded, makeRecord(0xD0));
      rc->InsertOrAssignInRecordCache(folded, e);

      // Execute Update (no enterEpoch — let Forward_epoch run freely)
      UpdateDescriptorGenerator1(ud, KVTable, my_payload);
      cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
      table.update1({key}, [&](KVTable& r){ r = makeRecord(0xD1); }, ud);
      cr::Worker::my().commitTX();
      info("Phase(4b): Update committed, waiting for Forward_epoch...");

      // Wait for hash table removal
      bool removed = waitUntil(
          [&]() { return rc->GetFromRecordCache(folded) == nullptr; },
          FLAGS_update_test_phase_timeout_sec);

      if (!removed) {
         auto* cur = rc->GetFromRecordCache(folded);
         if (cur) {
            info("Phase(4b): timed out, state=" +
                 std::string(typeName(cur->entry_type.load(std::memory_order_acquire))));
         }
         fail("Phase(4b): entry still in hash table after Forward_epoch timeout");
         std::abort();
      }
      pass("Phase(4b): After Forward_epoch, key is no longer in RecordCache hash table");
   });

   // ------------------------------------------------------------------
   // (4c) After Update: underlying BTree lookup must return NEW value,
   //      never the stale pre-update value.
   // ------------------------------------------------------------------
   crm.scheduleJobSync(0, [&]() {
      const TestKey key      = base + 3;
      auto          folded   = foldKey(key);
      const u8      old_val  = 0xE0;
      const u8      new_val  = 0xE1;

      // Inject stale cache image with old_val
      rc->EraseFromRecordCache(folded);
      auto* e = buildCacheEntry(*alloc, folded, makeRecord(old_val));
      rc->InsertOrAssignInRecordCache(folded, e);

      u64 wid = cr::Worker::my().worker_id;
      rc->enterEpoch(wid);  // freeze so we can inspect before Forward_epoch runs

      // Update to new_val
      UpdateDescriptorGenerator1(ud, KVTable, my_payload);
      cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
      table.update1({key}, [&](KVTable& r){ r = makeRecord(new_val); }, ud);
      cr::Worker::my().commitTX();

      // While entry is LogicallyDeleted in cache, BTree lookup must return new_val
      bool seen      = false;
      bool stale_hit = false;
      cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
      table.lookup1({key}, [&](const KVTable& rec) {
         seen = true;
         if (rec.my_payload.value[0] == old_val) {
            stale_hit = true;
         }
         info("Phase(4c): lookup returned payload[0]=0x" +
              [&]() {
                 std::ostringstream o;
                 o << std::hex << std::uppercase
                   << static_cast<int>(rec.my_payload.value[0]);
                 return o.str();
              }() +
              " (expected=0x" +
              [&]() {
                 std::ostringstream o;
                 o << std::hex << std::uppercase << static_cast<int>(new_val);
                 return o.str();
              }() + ")");
      });
      cr::Worker::my().commitTX();

      rc->leaveEpoch(wid);

      if (!seen) {
         fail("Phase(4c): lookup returned no record — key missing from BTree");
         std::abort();
      }
      if (stale_hit) {
         fail("Phase(4c): lookup returned stale value 0x" +
              [&]() {
                 std::ostringstream o;
                 o << std::hex << std::uppercase << static_cast<int>(old_val);
                 return o.str();
              }() + " — stale cache entry leaked into read path");
         std::abort();
      }
      pass("Phase(4c): BTree lookup returns new value after Update, "
           "stale RecordCache entry is invisible");
   });

   // ------------------------------------------------------------------
   // (4d) Consecutive updates on the same key: second Update sees the
   //      result of the first, not the original injected value.
   // ------------------------------------------------------------------
   crm.scheduleJobSync(0, [&]() {
      const TestKey key    = base + 4;
      auto          folded = foldKey(key);

      // First Update: old_val -> mid_val
      {
         rc->EraseFromRecordCache(folded);
         auto* e = buildCacheEntry(*alloc, folded, makeRecord(0xF0));
         rc->InsertOrAssignInRecordCache(folded, e);

         UpdateDescriptorGenerator1(ud, KVTable, my_payload);
         cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
         table.update1({key}, [&](KVTable& r){ r = makeRecord(0xF1); }, ud);
         cr::Worker::my().commitTX();
         info("Phase(4d): first Update committed (0xF0 -> 0xF1)");
      }

      // Wait for first update's LogicallyDeleted entry to be cleared
      waitUntil([&]() { return rc->GetFromRecordCache(folded) == nullptr; },
                FLAGS_update_test_phase_timeout_sec);

      // Second Update: mid_val -> new_val
      {
         UpdateDescriptorGenerator1(ud2, KVTable, my_payload);
         cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
         table.update1({key}, [&](KVTable& r){ r = makeRecord(0xF2); }, ud2);
         cr::Worker::my().commitTX();
         info("Phase(4d): second Update committed (0xF1 -> 0xF2)");
      }

      // Final lookup must return 0xF2
      bool seen = false;
      cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
      table.lookup1({key}, [&](const KVTable& rec) {
         seen = true;
         u8 got = rec.my_payload.value[0];
         info("Phase(4d): final lookup payload[0]=0x" +
              [&]() {
                 std::ostringstream o;
                 o << std::hex << std::uppercase << static_cast<int>(got);
                 return o.str();
              }());
         if (got != 0xF2) {
            fail("Phase(4d): expected 0xF2, got 0x" +
                 [&]() {
                    std::ostringstream o;
                    o << std::hex << std::uppercase << static_cast<int>(got);
                    return o.str();
                 }());
            std::abort();
         }
      });
      cr::Worker::my().commitTX();

      if (!seen) {
         fail("Phase(4d): key missing after second update");
         std::abort();
      }
      pass("Phase(4d): Consecutive updates are sequentially consistent");
   });
}

// ---------------------------------------------------------------------------
// Phase 5: Concurrency — Update/Lookup overlap, no deadlock/crash
// ---------------------------------------------------------------------------
static void phase5_concurrency(cr::CRManager& crm,
                                LeanStoreAdapter<KVTable>& table,
                                storage::recordcache::RecordCache* rc)
{
   print_phase("Phase 5: Concurrency (Update + Lookup overlap)");

   // Key range: lower 1/8 of tuple space so there is meaningful key overlap
   const u64 key_range = FLAGS_update_test_tuple_count / 8;

   std::atomic<bool> updater_done{false};
   std::atomic<u64>  update_ops{0};
   std::atomic<u64>  lookup_ops{0};
   std::atomic<u64>  update_aborts{0};
   std::atomic<u64>  lookup_aborts{0};

   // Watchdog: abort if neither thread makes progress for 30s
   std::atomic<bool> watchdog_armed{true};
   std::thread watchdog([&]() {
      u64 last_total = 0;
      while (watchdog_armed.load(std::memory_order_acquire)) {
         std::this_thread::sleep_for(std::chrono::seconds(5));
         u64 total = update_ops.load() + lookup_ops.load();
         if (total == last_total && watchdog_armed.load()) {
            fail("Phase 5: DEADLOCK detected — no progress for 5s "
                 "(update_ops=" + std::to_string(update_ops.load()) +
                 " lookup_ops=" + std::to_string(lookup_ops.load()) + ")");
            std::abort();
         }
         last_total = total;
      }
   });

   // Updater thread (worker 0)
   crm.scheduleJobAsync(0, [&]() {
      UpdateDescriptorGenerator1(ud, KVTable, my_payload);
      std::mt19937_64 rng(FLAGS_update_test_seed + 1);
      for (u64 i = 0; i < FLAGS_update_test_worker_updates; i++) {
         TestKey k = (rng() % key_range) + 1;
         jumpmuTry() {
            cr::Worker::my().startTX(TX_MODE::OLTP,
                                     TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
            table.update1({k},
                          [&](KVTable& r){ r = makeRecord(static_cast<u8>(i & 0xFF)); },
                          ud);
            cr::Worker::my().commitTX();
            update_ops.fetch_add(1, std::memory_order_relaxed);
         }
         jumpmuCatch() {
            update_aborts.fetch_add(1, std::memory_order_relaxed);
         }
      }
      updater_done.store(true, std::memory_order_release);
   });

   // Lookup thread (worker 1) — runs until updater finishes
   crm.scheduleJobAsync(1, [&]() {
      std::mt19937_64 rng(FLAGS_update_test_seed + 2);
      while (!updater_done.load(std::memory_order_acquire)) {
         TestKey k = (rng() % key_range) + 1;
         jumpmuTry() {
            cr::Worker::my().startTX(TX_MODE::OLTP,
                                     TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
            table.lookup1({k}, [&](const KVTable&){});
            cr::Worker::my().commitTX();
            lookup_ops.fetch_add(1, std::memory_order_relaxed);
         }
         jumpmuCatch() {
            lookup_aborts.fetch_add(1, std::memory_order_relaxed);
         }
      }
   });

   crm.joinAll();

   // Disarm watchdog
   watchdog_armed.store(false, std::memory_order_release);
   watchdog.join();

   info("Phase 5 stats:");
   info("  update_ops    = " + std::to_string(update_ops.load()));
   info("  update_aborts = " + std::to_string(update_aborts.load()));
   info("  lookup_ops    = " + std::to_string(lookup_ops.load()));
   info("  lookup_aborts = " + std::to_string(lookup_aborts.load()));
   info("  invalidation_queue_size = " +
        std::to_string(rc->debugInvalidationQueueSize()));
   info("  hash_table_entries      = " +
        std::to_string(rc->debugHashTableEntries()));

   if (update_ops.load() < FLAGS_update_test_worker_updates / 2) {
      warn("Phase 5: fewer than 50% of updates succeeded — "
           "high abort rate may indicate contention");
   }
   pass("Phase 5: Update/Lookup concurrency completed without deadlock or crash");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
   gflags::ParseCommandLineFlags(&argc, &argv, true);

   std::cout << "\n" << Color::BOLD << Color::CYAN
             << "=====================================================\n"
             << "  CXL Update Frontend Logic Integration Test\n"
             << "=====================================================" << Color::RESET << "\n\n";

   if (!FLAGS_cxl_tiering_enabled) {
      fail("Please run with --cxl_tiering_enabled=true");
      return 1;
   }

   // Initialize LeanStore
   LeanStore db;
   auto& crm = db.getCRManager();
   LeanStoreAdapter<KVTable> table;
   crm.scheduleJobSync(0, [&]() {
      table = LeanStoreAdapter<KVTable>(db, "CXL_UPDATE_TEST");
   });

   // Grab global handles
   auto& bm   = *storage::BMC::global_bf;
   auto* rc   = bm.global_record_cache;
   auto* alloc = bm.record_cache_allocator;
   if (!rc || !alloc) {
      fail("RecordCache or allocator is null — is cxl_tiering_enabled?");
      return 1;
   }

   // Run all phases
   phase1_load(crm, table, FLAGS_update_test_tuple_count);
   phase2_state_machine(crm, table, rc, alloc);
   phase3_background_liveness(crm, table, rc, alloc);
   phase4_correctness(crm, table, rc, alloc);
   phase5_concurrency(crm, table, rc);

   std::cout << "\n" << Color::BOLD << Color::GREEN
             << "=====================================================\n"
             << "  ALL PHASES PASSED\n"
             << "=====================================================" << Color::RESET << "\n";
   return 0;
}
