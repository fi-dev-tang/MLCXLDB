#include "../frontend/shared/LeanStoreAdapter.hpp"
#include "../frontend/shared/TwoTreeLeanStoreAdapter.hpp"
#include "../frontend/shared/BfTreeLeanStoreAdapter.hpp"
#include "../frontend/tpc-c/Schema.hpp"
#include "../frontend/tpc-c/TPCCWorkload.hpp"

#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/concurrency-recovery/CRMG.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/storage/buffer-manager/BufferManager.hpp"
#include "leanstore/utils/Misc.hpp"

#include <gflags/gflags.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace leanstore;
using u64 = std::uint64_t;

DEFINE_string(test_admission_mode, "two_level",
              "Mode: lru | page_only | two_level | tiered-indexing-zxj | hybried-tier-asplos2025 | bf-tree");
DEFINE_bool(test_load_data, true, "Load TPC-C data before run");
DEFINE_bool(test_verify_data, false, "Verify TPC-C data after load");
DEFINE_uint64(test_warmup_seconds, 30, "Warmup phase seconds");
DEFINE_uint64(test_measure_seconds, 60, "Measured phase seconds");
DEFINE_uint64(test_progress_interval_sec, 5, "Progress print interval in seconds");

DEFINE_int64(tpcc_warehouse_count, 1, "");
DEFINE_int32(tpcc_abort_pct, 0, "");
DEFINE_bool(tpcc_warehouse_affinity, false, "");
DEFINE_bool(tpcc_remove, true, "");
DEFINE_bool(order_wdc_index, true, "");
DEFINE_uint32(tpcc_threads, 0, "");

namespace Color {
const char* RESET   = "\033[0m";
const char* GREEN   = "\033[32m";
const char* YELLOW  = "\033[33m";
const char* CYAN    = "\033[36m";
const char* MAGENTA = "\033[35m";
const char* BOLD    = "\033[1m";
}

static void print_info(const std::string& msg) { std::cout << Color::CYAN << "[INFO] " << Color::RESET << msg << "\n"; }
static void print_warn(const std::string& msg) { std::cout << Color::YELLOW << "[WARN] " << Color::RESET << msg << "\n"; }
static void print_phase(const std::string& msg)
{
   std::cout << "\n" << Color::BOLD << Color::MAGENTA
             << "========================================\n"
             << "  " << msg << "\n"
             << "========================================" << Color::RESET << "\n";
}

struct DiagSnapshot {
   u64 record_cache_hit = 0;
   u64 record_cache_miss = 0;
   u64 dram_bp_hit = 0;
   u64 cxl_bp_hit = 0;
   u64 ssd_miss = 0;
   u64 promotions = 0;
   u64 demotions = 0;
   u64 evictions = 0;

   static DiagSnapshot capture(leanstore::storage::BufferManager& bm) {
      DiagSnapshot s;
      s.record_cache_hit = bm.diag.record_cache_hit.load(std::memory_order_relaxed);
      s.record_cache_miss = bm.diag.record_cache_miss.load(std::memory_order_relaxed);
      s.dram_bp_hit = bm.diag.dram_buffer_pool_hit.load(std::memory_order_relaxed);
      s.cxl_bp_hit = bm.diag.cxl_buffer_pool_hit.load(std::memory_order_relaxed);
      s.ssd_miss = bm.diag.ssd_miss.load(std::memory_order_relaxed);
      s.promotions = bm.diag.cxl_to_dram_promotions.load(std::memory_order_relaxed);
      s.demotions = bm.diag.dram_to_cxl_demotions.load(std::memory_order_relaxed);
      s.evictions = bm.diag.evictions.load(std::memory_order_relaxed);
      return s;
   }

   DiagSnapshot delta(const DiagSnapshot& before) const {
      DiagSnapshot d;
      d.record_cache_hit = record_cache_hit - before.record_cache_hit;
      d.record_cache_miss = record_cache_miss - before.record_cache_miss;
      d.dram_bp_hit = dram_bp_hit - before.dram_bp_hit;
      d.cxl_bp_hit = cxl_bp_hit - before.cxl_bp_hit;
      d.ssd_miss = ssd_miss - before.ssd_miss;
      d.promotions = promotions - before.promotions;
      d.demotions = demotions - before.demotions;
      d.evictions = evictions - before.evictions;
      return d;
   }
};

struct TPCCPhaseStats {
   u64 tx = 0;
   u64 tx_abort = 0;
   double secs = 0.0;
   DiagSnapshot diag_begin;
   DiagSnapshot diag_end;
};

template <template <typename> typename AdapterT>
static TPCCPhaseStats run_tpcc_phase(cr::CRManager& crm,
                                     TPCCWorkload<AdapterT>& tpcc,
                                     leanstore::storage::BufferManager& bm,
                                     leanstore::TX_ISOLATION_LEVEL isolation_level,
                                     u64 exec_threads,
                                     u64 duration_seconds)
{
   std::atomic<bool> keep_running{true};
   std::atomic<u64> running_threads{0};
   std::atomic<u64> tx{0};
   std::atomic<u64> tx_abort{0};

   DiagSnapshot diag_begin = DiagSnapshot::capture(bm);
   DiagSnapshot last_diag = diag_begin;

   auto t0 = std::chrono::high_resolution_clock::now();
   auto last_progress = t0;

   for (u64 t_i = 0; t_i < exec_threads; t_i++) {
      crm.scheduleJobAsync(t_i, [&, t_i]() {
         running_threads.fetch_add(1, std::memory_order_relaxed);
         cr::Worker::my().startTX(leanstore::TX_MODE::OLTP, isolation_level);
         tpcc.prepare();
         cr::Worker::my().commitTX();
         while (keep_running.load(std::memory_order_relaxed)) {
            jumpmuTry()
            {
               cr::Worker::my().startTX(leanstore::TX_MODE::OLTP, isolation_level);
               u32 w_id = FLAGS_tpcc_warehouse_affinity ? (t_i + 1) : tpcc.urand(1, FLAGS_tpcc_warehouse_count);
               tpcc.tx(w_id);
               if (FLAGS_tpcc_abort_pct && tpcc.urand(0, 100) <= FLAGS_tpcc_abort_pct) {
                  cr::Worker::my().abortTX();
               } else {
                  cr::Worker::my().commitTX();
               }
               tx.fetch_add(1, std::memory_order_relaxed);
            }
            jumpmuCatch()
            {
               tx_abort.fetch_add(1, std::memory_order_relaxed);
            }
         }
         cr::Worker::my().shutdown();
         running_threads.fetch_sub(1, std::memory_order_relaxed);
      });
   }

   u64 last_tx = 0;
   u64 last_abort = 0;

   while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const auto now = std::chrono::high_resolution_clock::now();
      const double elapsed = std::chrono::duration<double>(now - t0).count();
      if (FLAGS_test_progress_interval_sec > 0 &&
          std::chrono::duration_cast<std::chrono::seconds>(now - last_progress).count() >=
              static_cast<s64>(FLAGS_test_progress_interval_sec)) {
         const u64 cur_tx = tx.load(std::memory_order_relaxed);
         const u64 cur_abort = tx_abort.load(std::memory_order_relaxed);
         const double interval_secs = std::chrono::duration<double>(now - last_progress).count();
         const double interval_tps = interval_secs > 0 ? (cur_tx - last_tx) / interval_secs : 0;

         DiagSnapshot cur_diag = DiagSnapshot::capture(bm);
         DiagSnapshot d = cur_diag.delta(last_diag);
         const u64 total_bp = d.dram_bp_hit + d.cxl_bp_hit + d.ssd_miss;
         const double dram_hr = total_bp > 0 ? 100.0 * d.dram_bp_hit / total_bp : 0;
         const double cxl_hr = total_bp > 0 ? 100.0 * d.cxl_bp_hit / total_bp : 0;
         const double ssd_mr = total_bp > 0 ? 100.0 * d.ssd_miss / total_bp : 0;

         char buf[512];
         std::snprintf(buf, sizeof(buf),
             "t=%.0fs | TPS=%.0f | tx_interval=%llu abort_interval=%llu | "
             "DRAM=%.1f%% CXL=%.1f%% SSD=%.1f%% | promo=%llu demo=%llu evict=%llu",
             elapsed, interval_tps,
             (unsigned long long)(cur_tx - last_tx),
             (unsigned long long)(cur_abort - last_abort),
             dram_hr, cxl_hr, ssd_mr,
             (unsigned long long)d.promotions,
             (unsigned long long)d.demotions,
             (unsigned long long)d.evictions);
         print_info(std::string(buf));

         last_tx = cur_tx;
         last_abort = cur_abort;
         last_diag = cur_diag;
         last_progress = now;
      }
      if (elapsed >= static_cast<double>(duration_seconds)) {
         break;
      }
   }

   keep_running.store(false, std::memory_order_relaxed);
   while (running_threads.load(std::memory_order_relaxed)) {
   }
   crm.joinAll();

   TPCCPhaseStats s;
   s.tx = tx.load(std::memory_order_relaxed);
   s.tx_abort = tx_abort.load(std::memory_order_relaxed);
   s.secs = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
   s.diag_begin = diag_begin;
   s.diag_end = DiagSnapshot::capture(bm);
   return s;
}

int main(int argc, char** argv)
{
   gflags::SetUsageMessage("TPC-C comparison runner (phase-based)");
   gflags::ParseCommandLineFlags(&argc, &argv, true);
   ensure(FLAGS_tpcc_warehouse_count > 0);
   ensure(FLAGS_tpcc_threads <= FLAGS_worker_threads || FLAGS_tpcc_threads == 0);

   if (FLAGS_test_admission_mode != "lru" &&
       FLAGS_test_admission_mode != "page_only" &&
       FLAGS_test_admission_mode != "two_level" &&
       FLAGS_test_admission_mode != "tiered-indexing-zxj" &&
       FLAGS_test_admission_mode != "hybried-tier-asplos2025" &&
       FLAGS_test_admission_mode != "bf-tree") {
      print_warn("Unsupported mode: " + FLAGS_test_admission_mode);
      return 1;
   }

   // For bf-tree and tiered-indexing-zxj, our adapters handle tiering internally.
   // Use "lru" as the underlying admission mode so the old policy-level simulation
   // (BFTreeAdmissionPolicy / TieredIndexingPolicy) is NOT triggered on B+Tree accesses.
   if (FLAGS_test_admission_mode == "bf-tree" || FLAGS_test_admission_mode == "tiered-indexing-zxj") {
      FLAGS_admission_mode = "lru";
   } else {
      FLAGS_admission_mode = FLAGS_test_admission_mode;
   }
   if (FLAGS_test_admission_mode == "lru" || FLAGS_test_admission_mode == "page_only") {
      FLAGS_enable_record_cache = false;
      FLAGS_dram_recordcache_gib = 0.0;
      FLAGS_forward_epoch_thread = 0;
      FLAGS_sieve_eviction_thread = 0;
      FLAGS_record_cache_promote_thread = 0;
   }
   if (FLAGS_test_admission_mode == "hybried-tier-asplos2025") {
      FLAGS_enable_record_cache = false;
      FLAGS_dram_recordcache_gib = 0.0;
      FLAGS_forward_epoch_thread = 0;
      FLAGS_sieve_eviction_thread = 0;
      FLAGS_record_cache_promote_thread = 0;
      if (FLAGS_two_level_admission_threads == 0) {
         FLAGS_two_level_admission_threads = 1;
      }
   }
   // bf-tree mini-page IS the variable-length buffer pool (paper §3.1 + §4 are one
   // structure). Transfer the DRAM BP budget to mini-page and zero LeanStore's
   // DRAM BP so the two don't double-occupy DRAM. CXL becomes the sole page tier;
   // index pages fallback via jumpmuCatch (BufferManager.cpp:917-920). LRU
   // admission has no DRAM target in this mode, so threads stay 0.
   double bf_tree_mini_page_gib = 0.0;
   if (FLAGS_test_admission_mode == "bf-tree") {
      FLAGS_enable_record_cache = false;
      FLAGS_forward_epoch_thread = 0;
      FLAGS_sieve_eviction_thread = 0;
      FLAGS_record_cache_promote_thread = 0;
      FLAGS_two_level_admission_threads = 0;
      const double bf_bp_gib = FLAGS_dram_buffer_pool_gib * 0.25;  // 25% for BTree BP (matches two_level split)
      bf_tree_mini_page_gib      = FLAGS_dram_buffer_pool_gib - bf_bp_gib;
      FLAGS_dram_buffer_pool_gib = bf_bp_gib;
   }
   if (FLAGS_test_admission_mode == "tiered-indexing-zxj") {
      FLAGS_enable_record_cache = false;
      FLAGS_forward_epoch_thread = 0;
      FLAGS_sieve_eviction_thread = 0;
      FLAGS_record_cache_promote_thread = 0;
      FLAGS_two_level_admission_threads = 0;
   }
   if (FLAGS_test_admission_mode == "lru") {
      FLAGS_two_level_admission_threads = 0;
   } else if (FLAGS_test_admission_mode == "page_only") {
      if (FLAGS_two_level_admission_threads == 0) {
         FLAGS_two_level_admission_threads = 1;
      }
   }

   print_phase("TPC-C Compare Test");
   print_info("mode=" + FLAGS_test_admission_mode);
   print_info("warehouse=" + std::to_string(FLAGS_tpcc_warehouse_count));
   print_info("worker_threads=" + std::to_string(FLAGS_worker_threads));
   print_info("warmup=" + std::to_string(FLAGS_test_warmup_seconds) + "s"
              ", measure=" + std::to_string(FLAGS_test_measure_seconds) + "s");

   LeanStore::addS64Flag("TPC_SCALE", &FLAGS_tpcc_warehouse_count);
   LeanStore db;
   auto& crm = db.getCRManager();

   const bool is_3t_mode = (FLAGS_test_admission_mode == "tiered-indexing-zxj");
   const bool is_bf_tree_mode = (FLAGS_test_admission_mode == "bf-tree");

   leanstore::TX_ISOLATION_LEVEL isolation_level = leanstore::parseIsolationLevel(FLAGS_isolation_level);
   const bool should_tpcc_driver_handle_isolation_anomalies =
       isolation_level < leanstore::TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION;
   const u64 exec_threads = FLAGS_tpcc_threads ? FLAGS_tpcc_threads : FLAGS_worker_threads;
   print_info("exec_threads=" + std::to_string(exec_threads));

   auto& bm = db.getBufferManager();

   // =========================================================================
   // Helper lambda: load, warmup, measure for a given adapter type
   // =========================================================================
   auto run_all_phases = [&](auto& tpcc) {
      if (!FLAGS_recover && FLAGS_test_load_data) {
         print_phase("Phase 1: Load");
         crm.scheduleJobSync(0, [&]() {
            cr::Worker::my().startTX(leanstore::TX_MODE::INSTANTLY_VISIBLE_BULK_INSERT);
            tpcc.loadItem();
            tpcc.loadWarehouse();
            cr::Worker::my().commitTX();
         });
         std::atomic<u32> g_w_id = 1;
         for (u32 t_i = 0; t_i < FLAGS_worker_threads; t_i++) {
            crm.scheduleJobAsync(t_i, [&]() {
               while (true) {
                  u32 w_id = g_w_id++;
                  if (w_id > static_cast<u32>(FLAGS_tpcc_warehouse_count)) return;
                  cr::Worker::my().startTX(leanstore::TX_MODE::INSTANTLY_VISIBLE_BULK_INSERT);
                  tpcc.loadStock(w_id);
                  tpcc.loadDistrinct(w_id);
                  for (Integer d_id = 1; d_id <= 10; d_id++) {
                     tpcc.loadCustomer(w_id, d_id);
                     tpcc.loadOrders(w_id, d_id);
                  }
                  cr::Worker::my().commitTX();
               }
            });
         }
         crm.joinAll();
         if (FLAGS_test_verify_data) {
            print_phase("Verify");
            crm.scheduleJobSync(0, [&]() {
               cr::Worker::my().startTX(leanstore::TX_MODE::OLTP);
               tpcc.verifyItems();
               cr::Worker::my().commitTX();
            });
         }
      }

      // Deferred background-thread start, mirrors experiment1_ycsb_d.cpp:1092.
      // When --delay_admission_recordcache_threads_start=true, BufferManager skips
      // starting the two-level admission + RecordCache threads at init so that
      // Phase-1 bulk-insert traffic does not pollute the CMS / candidate map.
      // Without this call the threads stay off for the entire run, producing
      // 0 promotions and 0% RecordCache hit (observed in ASAN run 150736).
      // setLogicalCapacityFromEntrySize is intentionally NOT called: TPC-C
      // records range from ~30B (neworder) to ~700B (customer), so there is
      // no single representative avg_entry_bytes — fill ratio falls back to
      // allocator.getUsageRatio() which is byte-accurate.
      if (FLAGS_cxl_tiering_enabled && FLAGS_delay_admission_recordcache_threads_start) {
         print_info("Enabling deferred background threads (admission + RecordCache)...");
         storage::BMC::global_bf->enableAdmissionAndRecordCacheThreads();
         print_info("Deferred background threads enabled.");
      }

      if (FLAGS_test_warmup_seconds > 0) {
         print_phase("Phase 2: Warmup");
         TPCCPhaseStats warmup = run_tpcc_phase(crm, tpcc, bm, isolation_level, exec_threads, FLAGS_test_warmup_seconds);
         print_info("warmup_tx=" + std::to_string(warmup.tx) +
                    ", warmup_abort=" + std::to_string(warmup.tx_abort) +
                    ", warmup_tps=" + std::to_string(warmup.tx / std::max(0.0001, warmup.secs)));
      }

      print_phase("Phase 3: Measure");
      return run_tpcc_phase(crm, tpcc, bm, isolation_level, exec_threads, FLAGS_test_measure_seconds);
   };

   TPCCPhaseStats measured;

   if (is_3t_mode) {
      // 3T mode: each table gets a pair of hot+cold B+Trees
      TwoTreeLeanStoreAdapter<warehouse_t> warehouse;
      TwoTreeLeanStoreAdapter<district_t> district;
      TwoTreeLeanStoreAdapter<customer_t> customer;
      TwoTreeLeanStoreAdapter<customer_wdl_t> customerwdl;
      TwoTreeLeanStoreAdapter<history_t> history;
      TwoTreeLeanStoreAdapter<neworder_t> neworder;
      TwoTreeLeanStoreAdapter<order_t> order;
      TwoTreeLeanStoreAdapter<order_wdc_t> order_wdc;
      TwoTreeLeanStoreAdapter<orderline_t> orderline;
      TwoTreeLeanStoreAdapter<item_t> item;
      TwoTreeLeanStoreAdapter<stock_t> stock;

      const double total_hot_gib = FLAGS_dram_buffer_pool_gib + FLAGS_dram_recordcache_gib;
      const double per_table_gib = total_hot_gib / 11.0;

      crm.scheduleJobSync(0, [&]() {
         warehouse = TwoTreeLeanStoreAdapter<warehouse_t>(db, "warehouse", per_table_gib);
         district = TwoTreeLeanStoreAdapter<district_t>(db, "district", per_table_gib);
         customer = TwoTreeLeanStoreAdapter<customer_t>(db, "customer", per_table_gib);
         customerwdl = TwoTreeLeanStoreAdapter<customer_wdl_t>(db, "customerwdl", per_table_gib);
         history = TwoTreeLeanStoreAdapter<history_t>(db, "history", per_table_gib);
         neworder = TwoTreeLeanStoreAdapter<neworder_t>(db, "neworder", per_table_gib);
         order = TwoTreeLeanStoreAdapter<order_t>(db, "order", per_table_gib);
         order_wdc = TwoTreeLeanStoreAdapter<order_wdc_t>(db, "order_wdc", per_table_gib);
         orderline = TwoTreeLeanStoreAdapter<orderline_t>(db, "orderline", per_table_gib);
         item = TwoTreeLeanStoreAdapter<item_t>(db, "item", per_table_gib);
         stock = TwoTreeLeanStoreAdapter<stock_t>(db, "stock", per_table_gib);
      });

      TPCCWorkload<TwoTreeLeanStoreAdapter> tpcc(
          warehouse, district, customer, customerwdl, history, neworder, order, order_wdc, orderline, item, stock,
          FLAGS_order_wdc_index, FLAGS_tpcc_warehouse_count, FLAGS_tpcc_remove,
          should_tpcc_driver_handle_isolation_anomalies, FLAGS_tpcc_warehouse_affinity);

      measured = run_all_phases(tpcc);

      print_info("[3T] Per-table stats:");
      warehouse.report(); district.report(); customer.report();
      customerwdl.report(); history.report(); neworder.report();
      order.report(); order_wdc.report(); orderline.report();
      item.report(); stock.report();
   } else if (is_bf_tree_mode) {
      // BfTree mode: single B+Tree per table with in-DRAM mini-page overlay
      BfTreeLeanStoreAdapter<warehouse_t> warehouse;
      BfTreeLeanStoreAdapter<district_t> district;
      BfTreeLeanStoreAdapter<customer_t> customer;
      BfTreeLeanStoreAdapter<customer_wdl_t> customerwdl;
      BfTreeLeanStoreAdapter<history_t> history;
      BfTreeLeanStoreAdapter<neworder_t> neworder;
      BfTreeLeanStoreAdapter<order_t> order;
      BfTreeLeanStoreAdapter<order_wdc_t> order_wdc;
      BfTreeLeanStoreAdapter<orderline_t> orderline;
      BfTreeLeanStoreAdapter<item_t> item;
      BfTreeLeanStoreAdapter<stock_t> stock;

      // Use the captured pre-zero mini-page budget (see dispatch block above);
      // FLAGS_dram_buffer_pool_gib has already been zeroed and would yield 0 here.
      const double per_table_gib = bf_tree_mini_page_gib / 11.0;

      crm.scheduleJobSync(0, [&]() {
         warehouse = BfTreeLeanStoreAdapter<warehouse_t>(db, "warehouse", per_table_gib);
         district = BfTreeLeanStoreAdapter<district_t>(db, "district", per_table_gib);
         customer = BfTreeLeanStoreAdapter<customer_t>(db, "customer", per_table_gib);
         customerwdl = BfTreeLeanStoreAdapter<customer_wdl_t>(db, "customerwdl", per_table_gib);
         history = BfTreeLeanStoreAdapter<history_t>(db, "history", per_table_gib);
         neworder = BfTreeLeanStoreAdapter<neworder_t>(db, "neworder", per_table_gib);
         order = BfTreeLeanStoreAdapter<order_t>(db, "order", per_table_gib);
         order_wdc = BfTreeLeanStoreAdapter<order_wdc_t>(db, "order_wdc", per_table_gib);
         orderline = BfTreeLeanStoreAdapter<orderline_t>(db, "orderline", per_table_gib);
         item = BfTreeLeanStoreAdapter<item_t>(db, "item", per_table_gib);
         stock = BfTreeLeanStoreAdapter<stock_t>(db, "stock", per_table_gib);
      });

      TPCCWorkload<BfTreeLeanStoreAdapter> tpcc(
          warehouse, district, customer, customerwdl, history, neworder, order, order_wdc, orderline, item, stock,
          FLAGS_order_wdc_index, FLAGS_tpcc_warehouse_count, FLAGS_tpcc_remove,
          should_tpcc_driver_handle_isolation_anomalies, FLAGS_tpcc_warehouse_affinity);

      measured = run_all_phases(tpcc);

      print_info("[BfTree] Per-table stats:");
      warehouse.report(); district.report(); customer.report();
      customerwdl.report(); history.report(); neworder.report();
      order.report(); order_wdc.report(); orderline.report();
      item.report(); stock.report();
   } else {
      // Standard mode: single B+Tree per table
      LeanStoreAdapter<warehouse_t> warehouse;
      LeanStoreAdapter<district_t> district;
      LeanStoreAdapter<customer_t> customer;
      LeanStoreAdapter<customer_wdl_t> customerwdl;
      LeanStoreAdapter<history_t> history;
      LeanStoreAdapter<neworder_t> neworder;
      LeanStoreAdapter<order_t> order;
      LeanStoreAdapter<order_wdc_t> order_wdc;
      LeanStoreAdapter<orderline_t> orderline;
      LeanStoreAdapter<item_t> item;
      LeanStoreAdapter<stock_t> stock;

      crm.scheduleJobSync(0, [&]() {
         warehouse = LeanStoreAdapter<warehouse_t>(db, "warehouse");
         district = LeanStoreAdapter<district_t>(db, "district");
         customer = LeanStoreAdapter<customer_t>(db, "customer");
         customerwdl = LeanStoreAdapter<customer_wdl_t>(db, "customerwdl");
         history = LeanStoreAdapter<history_t>(db, "history");
         neworder = LeanStoreAdapter<neworder_t>(db, "neworder");
         order = LeanStoreAdapter<order_t>(db, "order");
         order_wdc = LeanStoreAdapter<order_wdc_t>(db, "order_wdc");
         orderline = LeanStoreAdapter<orderline_t>(db, "orderline");
         item = LeanStoreAdapter<item_t>(db, "item");
         stock = LeanStoreAdapter<stock_t>(db, "stock");
      });

      TPCCWorkload<LeanStoreAdapter> tpcc(
          warehouse, district, customer, customerwdl, history, neworder, order, order_wdc, orderline, item, stock,
          FLAGS_order_wdc_index, FLAGS_tpcc_warehouse_count, FLAGS_tpcc_remove,
          should_tpcc_driver_handle_isolation_anomalies, FLAGS_tpcc_warehouse_affinity);

      measured = run_all_phases(tpcc);
   }

   const double tps = measured.tx / std::max(0.0001, measured.secs);
   const double abort_rate = (measured.tx + measured.tx_abort) > 0
                                 ? 100.0 * static_cast<double>(measured.tx_abort) /
                                       static_cast<double>(measured.tx + measured.tx_abort)
                                 : 0.0;
   const double used_gib = (bm.inUsedPageIDCount() * EFFECTIVE_PAGE_SIZE / 1024.0 / 1024.0 / 1024.0);

   // Compute measured-phase tier stats
   DiagSnapshot d = measured.diag_end.delta(measured.diag_begin);
   const u64 total_bp_accesses = d.dram_bp_hit + d.cxl_bp_hit + d.ssd_miss;
   const double dram_hr = total_bp_accesses > 0 ? 100.0 * d.dram_bp_hit / total_bp_accesses : 0;
   const double cxl_hr = total_bp_accesses > 0 ? 100.0 * d.cxl_bp_hit / total_bp_accesses : 0;
   const double ssd_mr = total_bp_accesses > 0 ? 100.0 * d.ssd_miss / total_bp_accesses : 0;

   const u64 dram_total_pages = bm.getPoolSize();
   const u64 cxl_total_pages = bm.getCXLPoolSize();

   // Aggregate WAL bytes across all worker threads
   u64 total_wal_bytes = 0;
   for (auto& wc : leanstore::WorkerCounters::worker_counters) {
      total_wal_bytes += wc.wal_write_bytes.load(std::memory_order_relaxed);
   }
   const double wal_mib = total_wal_bytes / 1024.0 / 1024.0;

   print_phase("Final Summary");
   std::printf("%-28s %s\n", "Mode:", FLAGS_test_admission_mode.c_str());
   std::printf("%-28s %llu\n", "Warehouses:", (unsigned long long)FLAGS_tpcc_warehouse_count);
   std::printf("%-28s %llu\n", "Worker threads:", (unsigned long long)exec_threads);
   std::printf("%-28s %.2f s\n", "Measured duration:", measured.secs);
   std::printf("\n");
   std::printf("--- Throughput ---\n");
   std::printf("%-28s %llu\n", "Committed TX:", (unsigned long long)measured.tx);
   std::printf("%-28s %llu\n", "Aborted TX:", (unsigned long long)measured.tx_abort);
   std::printf("%-28s %.0f\n", "TPS:", tps);
   std::printf("%-28s %.2f%%\n", "Abort rate:", abort_rate);
   std::printf("\n");
   std::printf("--- Buffer Pool Tier Stats (measured phase) ---\n");
   std::printf("%-28s %llu\n", "Total BP accesses:", (unsigned long long)total_bp_accesses);
   std::printf("%-28s %llu (%.2f%%)\n", "DRAM BP hits:", (unsigned long long)d.dram_bp_hit, dram_hr);
   std::printf("%-28s %llu (%.2f%%)\n", "CXL BP hits:", (unsigned long long)d.cxl_bp_hit, cxl_hr);
   std::printf("%-28s %llu (%.2f%%)\n", "SSD misses:", (unsigned long long)d.ssd_miss, ssd_mr);
   if (d.record_cache_hit > 0 || d.record_cache_miss > 0) {
      const u64 rc_total = d.record_cache_hit + d.record_cache_miss;
      const double rc_hr = rc_total > 0 ? 100.0 * d.record_cache_hit / rc_total : 0;
      std::printf("%-28s %llu / %llu (%.2f%%)\n", "Record cache hit/miss:",
                  (unsigned long long)d.record_cache_hit,
                  (unsigned long long)rc_total, rc_hr);
   }
   std::printf("\n");
   std::printf("--- Migration & Eviction ---\n");
   std::printf("%-28s %llu\n", "CXL->DRAM promotions:", (unsigned long long)d.promotions);
   std::printf("%-28s %llu\n", "DRAM->CXL demotions:", (unsigned long long)d.demotions);
   std::printf("%-28s %llu\n", "Evictions (to SSD):", (unsigned long long)d.evictions);
   std::printf("\n");
   std::printf("--- Buffer Pool Capacity ---\n");
   std::printf("%-28s %llu pages (%.3f GiB)\n", "DRAM pool:",
               (unsigned long long)dram_total_pages,
               dram_total_pages * EFFECTIVE_PAGE_SIZE / 1024.0 / 1024.0 / 1024.0);
   if (cxl_total_pages > 0) {
      std::printf("%-28s %llu pages (%.3f GiB)\n", "CXL pool:",
                  (unsigned long long)cxl_total_pages,
                  cxl_total_pages * EFFECTIVE_PAGE_SIZE / 1024.0 / 1024.0 / 1024.0);
   }
   std::printf("%-28s %.3f GiB\n", "Used (page IDs allocated):", used_gib);
   std::printf("\n");
   std::printf("--- WAL ---\n");
   std::printf("%-28s %.2f MiB (%.2f MiB/s)\n", "WAL written:",
               wal_mib, wal_mib / std::max(0.001, measured.secs));
   std::printf("\n");

   std::cout << Color::GREEN << "[PASS] " << Color::RESET
             << "TPCC compare run finished.\n";
   return 0;
}
