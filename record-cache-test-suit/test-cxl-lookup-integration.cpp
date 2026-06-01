// =============================================================================
// test-cxl-lookup-integration.cpp
//
// Integration test for CXL-tiered lookup path:
//   Phase 1: Data loading (uniform insert) + CXL memory verification
//   Phase 2: DRAM vs CXL bandwidth/latency measurement
//   Phase 3: Two-Level-Zipfian lookup with admission control monitoring
//   Phase 4: Hit-rate statistics
//   Phase 5: Partition debug dump
//
// Build:  cmake target "cxl_lookup_test" in frontend/CMakeLists.txt
// Run:    ./build/frontend/cxl_lookup_test \
//           --cxl_tiering_enabled=true --dram_buffer_pool_gib=1 \
//           --dram_recordcache_gib=1 --cxl_gib=16 \
//           --cxl_dax_device_path=/dev/dax0.1 --worker_threads=4 \
//           --vi=true \
//           --pp_threads=1 --cxl_pp_threads=1 \
//           --two_level_admission_threads=1 \
//           --forward_epoch_thread=1 --sieve_eviction_thread=1 \
//           --record_cache_promote_thread=1 \
//           --ssd_path=/tmp/cxl_test_ssd --trunc=true --wal=false
// =============================================================================
#include "../frontend/shared/LeanStoreAdapter.hpp"
#include "../frontend/shared/Schema.hpp"
#include "Units.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/utils/Parallelize.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include "leanstore/storage/buffer-manager/BufferManager.hpp"
#include "leanstore/storage/record-cache/RecordCacheEntry.hpp"
#include "leanstore/storage/two-level-admission-control/TwoLevelAdmissionControl.hpp"
// -------------------------------------------------------------------------------------
#include <gflags/gflags.h>
// -------------------------------------------------------------------------------------
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <immintrin.h>  // _mm_clflush, _mm_mfence
// -------------------------------------------------------------------------------------
DEFINE_uint64(test_tuple_count, 0, "Total records to insert (0 = auto from target_gib)");
DEFINE_double(test_data_gib, 4.0, "Target data size in GiB");
DEFINE_double(test_load_gib, 0.0, "Target load size in GiB (preferred, overrides test_data_gib when > 0)");
DEFINE_uint64(test_total_lookups, 300000000ULL, "Total lookup queries");
DEFINE_uint64(test_warmup_lookups, 20000000ULL, "Warmup lookup queries before hit-rate measurement");
DEFINE_uint64(test_print_interval, 10000, "Print admission control stats every N lookups");
DEFINE_double(test_theta_page, 0.99, "Zipfian theta for inter-page skew");
DEFINE_double(test_theta_slot, 1.3, "Zipfian theta for intra-page skew");
DEFINE_uint64(test_records_per_page, 175, "Estimated records per BTree leaf page");
DEFINE_string(test_admission_log, "admission_control_log.csv", "Output file for admission stats");
DEFINE_string(test_hit_stats_log, "hit_stats.csv", "Output file for hit-rate stats");
// -------------------------------------------------------------------------------------
using namespace leanstore;
// -------------------------------------------------------------------------------------
using TestKey = u64;
using TestPayload = BytesPayload<8>;
using KVTable = Relation<TestKey, TestPayload>;

void print_info(const std::string& msg);

static double zipf_topk_mass(u64 n, double theta, u64 topk) {
   if (n == 0 || topk == 0) return 0.0;
   topk = std::min(topk, n);
   double denom = 0.0;
   for (u64 i = 1; i <= n; i++) denom += 1.0 / std::pow(static_cast<double>(i), theta);
   double numer = 0.0;
   for (u64 i = 1; i <= topk; i++) numer += 1.0 / std::pow(static_cast<double>(i), theta);
   return numer / denom;
}

void print_theory_bounds(u64 total_pages, u64 records_per_page, double theta_page) {
   constexpr u64 GIB = 1024ULL * 1024ULL * 1024ULL;
   const u64 dram_page_capacity =
      std::min<u64>(total_pages, static_cast<u64>(FLAGS_dram_buffer_pool_gib * GIB / storage::PAGE_SIZE));
   const double dram_page_hot_mass = zipf_topk_mass(total_pages, theta_page, dram_page_capacity);
   const double cxl_traffic_mass_upper = std::max(0.0, 1.0 - dram_page_hot_mass);

   const u64 cxl_pages = (total_pages > dram_page_capacity) ? (total_pages - dram_page_capacity) : 0;
   const u64 cxl_records = cxl_pages * records_per_page;
   const u64 rc_entry_bytes = sizeof(storage::recordcache::RecordCacheEntry) + sizeof(TestKey) + sizeof(TestPayload);
   const u64 rc_capacity_entries = static_cast<u64>(FLAGS_dram_recordcache_gib * GIB / rc_entry_bytes);
   const double rc_cover_cxl = (cxl_records > 0) ? std::min(1.0, (1.0 * rc_capacity_entries / cxl_records)) : 0.0;

   // Conservative bound: RecordCache only sees lookups that miss DRAM page residency.
   const double rc_hit_upper_conservative = cxl_traffic_mass_upper * rc_cover_cxl;

   std::ostringstream oss;
   oss << std::fixed << std::setprecision(4)
       << "Theory bounds: DRAM pages=" << dram_page_capacity << "/" << total_pages
       << " => page-hot-mass~" << (dram_page_hot_mass * 100.0) << "%, "
       << "CXL-traffic-upper~" << (cxl_traffic_mass_upper * 100.0) << "%, "
       << "RC-entry-bytes=" << rc_entry_bytes << ", RC-capacity~" << rc_capacity_entries
       << " entries, CXL-record-cover~" << (rc_cover_cxl * 100.0) << "%, "
       << "RC-hit-upper(conservative)~" << (rc_hit_upper_conservative * 100.0) << "%";
   print_info(oss.str());
}

// =============================================================================
// ANSI color helpers (same style as test-forward-epoch-thread.cpp)
// =============================================================================
namespace Color {
const char* RESET   = "\033[0m";
const char* RED     = "\033[31m";
const char* GREEN   = "\033[32m";
const char* YELLOW  = "\033[33m";
const char* BLUE    = "\033[34m";
const char* MAGENTA = "\033[35m";
const char* CYAN    = "\033[36m";
const char* BOLD    = "\033[1m";
}

void print_pass(const std::string& msg) { std::cout << Color::GREEN << "[PASS] " << Color::RESET << msg << "\n"; }
void print_fail(const std::string& msg) { std::cout << Color::RED << "[FAIL] " << Color::RESET << msg << "\n"; }
void print_info(const std::string& msg) { std::cout << Color::CYAN << "[INFO] " << Color::RESET << msg << "\n"; }
void print_warn(const std::string& msg) { std::cout << Color::YELLOW << "[WARN] " << Color::RESET << msg << "\n"; }
void print_phase(const std::string& msg) {
   std::cout << "\n" << Color::BOLD << Color::MAGENTA
             << "========================================\n"
             << "  " << msg << "\n"
             << "========================================" << Color::RESET << "\n";
}

// =============================================================================
// Two-Level Zipfian Generator
//
// Level 1: Zipfian(num_pages, theta_page)  -> which page
// Level 2: Zipfian(records_per_page, theta_slot) -> which slot within page
// Final key = page_rank * records_per_page + slot_rank
// =============================================================================
class ZipfianSampler {
   std::vector<double> cdf_;
public:
   ZipfianSampler() = default;
   void init(uint64_t n, double theta) {
      cdf_.resize(n);
      double sum = 0;
      for (uint64_t i = 0; i < n; i++) {
         sum += 1.0 / std::pow(static_cast<double>(i + 1), theta);
         cdf_[i] = sum;
      }
      double inv = 1.0 / sum;
      for (uint64_t i = 0; i < n; i++) cdf_[i] *= inv;
   }
   uint64_t sample(std::mt19937_64& rng) const {
      std::uniform_real_distribution<double> u(0.0, 1.0);
      double r = u(rng);
      auto it = std::lower_bound(cdf_.begin(), cdf_.end(), r);
      return static_cast<uint64_t>(std::distance(cdf_.begin(), it));
   }
   uint64_t size() const { return cdf_.size(); }
};

class TwoLevelZipfianGenerator {
   ZipfianSampler page_sampler_;
   ZipfianSampler slot_sampler_;
   uint64_t records_per_page_;
   uint64_t total_keys_;
public:
   TwoLevelZipfianGenerator() = default;

   void init(uint64_t num_pages, uint64_t records_per_page,
             double theta_page, double theta_slot) {
      records_per_page_ = records_per_page;
      total_keys_ = num_pages * records_per_page;
      print_info("Building page-level Zipfian CDF (" + std::to_string(num_pages) + " pages, theta=" +
                 std::to_string(theta_page) + ") ...");
      page_sampler_.init(num_pages, theta_page);
      print_info("Building slot-level Zipfian CDF (" + std::to_string(records_per_page) + " slots, theta=" +
                 std::to_string(theta_slot) + ") ...");
      slot_sampler_.init(records_per_page, theta_slot);
      print_info("Two-Level Zipfian generator ready.");
   }

   uint64_t next(std::mt19937_64& rng) const {
      uint64_t page = page_sampler_.sample(rng);
      uint64_t slot = slot_sampler_.sample(rng);
      uint64_t key = page * records_per_page_ + slot;
      return std::min(key, total_keys_ - 1);
   }

   uint64_t total_keys() const { return total_keys_; }
};

// =============================================================================
// Phase 1+2: Data Loading + CXL Memory Verification
// =============================================================================
void phase1_load_data(cr::CRManager& crm, LeanStoreAdapter<KVTable>& table, u64 tuple_count) {
   print_phase("Phase 1: Data Loading (" + std::to_string(tuple_count) + " records)");

   auto& bm = *storage::BMC::global_bf;
   const TX_MODE tx_type = TX_MODE::OLTP;

   auto t_begin = std::chrono::high_resolution_clock::now();
   const u64 report_interval = std::max<u64>(tuple_count / 20, 1ULL);

   utils::Parallelize::range(FLAGS_worker_threads, tuple_count, [&](u64 t_i, u64 begin, u64 end) {
      crm.scheduleJobAsync(t_i, [&, begin, end]() {
         for (u64 i = begin; i < end; i++) {
            TestPayload payload;
            utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(TestPayload));
            TestKey key = i;
            cr::Worker::my().startTX(tx_type, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
            table.insert({key}, {payload});
            cr::Worker::my().commitTX();

            if (t_i == 0 && i > begin && (i - begin) % report_interval == 0) {
               double pct = 100.0 * (i - begin) / (end - begin);
               u64 used_pages = bm.inUsedPageIDCount();
               double used_gib = used_pages * 1.0 * storage::PAGE_SIZE / 1024.0 / 1024.0 / 1024.0;
               std::ostringstream oss;
               oss << std::fixed << std::setprecision(1)
                   << "Worker 0: " << pct << "%, pages=" << used_pages
                   << " (" << used_gib << " GiB)";
               print_info(oss.str());
            }
         }
      });
   });
   crm.joinAll();

   auto t_end = std::chrono::high_resolution_clock::now();
   double secs = std::chrono::duration<double>(t_end - t_begin).count();

   u64 final_pages = bm.inUsedPageIDCount();
   double final_gib = final_pages * 1.0 * storage::PAGE_SIZE / 1024.0 / 1024.0 / 1024.0;
   print_info("Load complete: " + std::to_string(tuple_count) + " records, "
              + std::to_string(final_pages) + " pages (" + std::to_string(final_gib) + " GiB), "
              + std::to_string(secs) + " sec, "
              + std::to_string(tuple_count / secs / 1e6) + " Mtps");
}

// =============================================================================
// Phase 2: CXL Memory Verification + Bandwidth/Latency Measurement
// =============================================================================
void phase2_cxl_verification_and_bandwidth() {
   print_phase("Phase 2: CXL Memory Verification + Bandwidth Measurement");

   auto& bm = *storage::BMC::global_bf;
   storage::BufferFrame* dram_bfs = bm.getDRAMBFs();
   storage::BufferFrame* cxl_bfs  = bm.getCXLBFs();
   u64 dram_num = bm.getPoolSize();
   u64 cxl_num  = bm.getCXLPoolSize();

   // --- Step 1: Count active (non-FREE) frames in each tier ---
   // This will pollute CPU cache; we flush explicitly before any measurement.
   u64 dram_active = 0, cxl_active = 0;
   for (u64 i = 0; i < dram_num; i++) {
      if (dram_bfs[i].header.state != storage::BufferFrame::STATE::FREE) dram_active++;
   }
   if (cxl_bfs && cxl_num > 0) {
      for (u64 i = 0; i < cxl_num; i++) {
         if (cxl_bfs[i].header.state != storage::BufferFrame::STATE::FREE) cxl_active++;
      }
   }

   print_info("DRAM BufferFrames: total=" + std::to_string(dram_num) +
              ", active=" + std::to_string(dram_active) +
              " (" + std::to_string(100.0 * dram_active / dram_num) + "%)");

   if (cxl_bfs && cxl_num > 0) {
      print_info("CXL  BufferFrames: total=" + std::to_string(cxl_num) +
                 ", active=" + std::to_string(cxl_active) +
                 " (" + std::to_string(100.0 * cxl_active / cxl_num) + "%)");
      if (cxl_active > 0) {
         print_pass("CXL memory IS being used (" + std::to_string(cxl_active) + " active frames)");
      } else {
         print_warn("CXL memory has 0 active frames — pages may not have been demoted yet");
      }
   } else {
      print_warn("CXL buffer pool not available (cxl_bfs=nullptr or cxl_num=0)");
   }

   // --- Flush helper: evict every cache line in [ptr, ptr+bytes) ---
   auto flush_region = [](const void* ptr, u64 bytes) {
      const char* p = reinterpret_cast<const char*>(ptr);
      for (u64 i = 0; i < bytes; i += 64) {
         _mm_clflush(p + i);
      }
      _mm_mfence();
   };

   // --- Step 2: Sequential bandwidth measurement ---
   // We read EVERY cache line (64-byte stride) inside each BufferFrame so
   // the actual working set equals count * sizeof(BufferFrame).  For pools
   // larger than LLC (which is always the case), the prefetcher cannot hide
   // all latency, so the measured BW reflects real DRAM/CXL bandwidth.
   // Full-region clflush is skipped when pool > 256 MiB (working set >> LLC).
   constexpr u64 LLC_FLUSH_THRESHOLD = 256ULL * 1024 * 1024;

   auto measure_seq_bandwidth = [&flush_region](const char* label,
                                                 storage::BufferFrame* pool,
                                                 u64 count,
                                                 u64 flush_threshold) -> double {
      if (!pool || count == 0) return -1.0;

      const u64 total_bytes = count * sizeof(storage::BufferFrame);
      const u64 cls_per_frame = sizeof(storage::BufferFrame) / 64;

      if (total_bytes <= flush_threshold) {
         print_info(std::string(label) + " flushing " + std::to_string(total_bytes / 1024 / 1024)
                    + " MiB (pool <= LLC threshold) ...");
         flush_region(pool, total_bytes);
      } else {
         print_info(std::string(label) + " pool " + std::to_string(total_bytes / 1024 / 1024)
                    + " MiB >> LLC, skipping full clflush (working set self-evicts)");
      }

      volatile u64 sink = 0;
      auto t0 = std::chrono::high_resolution_clock::now();
      for (u64 i = 0; i < count; i++) {
         const u8* fp = reinterpret_cast<const u8*>(&pool[i]);
         for (u64 cl = 0; cl < cls_per_frame; cl++) {
            sink += *reinterpret_cast<const volatile u64*>(fp + cl * 64);
         }
      }
      auto t1 = std::chrono::high_resolution_clock::now();

      double secs = std::chrono::duration<double>(t1 - t0).count();
      double gib  = total_bytes / (1024.0 * 1024.0 * 1024.0);
      double bw   = gib / secs;

      std::ostringstream oss;
      oss << std::fixed << std::setprecision(2)
          << label << " seq full-frame scan: " << count << " frames, "
          << gib << " GiB, " << secs * 1000.0 << " ms, " << bw << " GiB/s"
          << "  (" << cls_per_frame << " cache-lines/frame)";
      print_info(oss.str());
      (void)sink;
      return bw;
   };

   double dram_seq_bw = measure_seq_bandwidth("DRAM", dram_bfs, dram_num, LLC_FLUSH_THRESHOLD);
   double cxl_seq_bw  = measure_seq_bandwidth("CXL ", cxl_bfs,  cxl_num,  LLC_FLUSH_THRESHOLD);

   // --- Step 3: Random latency measurement ---
   // Strategy: pre-generate random indices, flush target cache lines, then
   // measure load + clflush per iteration.  The clflush after each load
   // ensures the next access to the same line will miss cache.
   // Returns average latency per access (ns); -1 if pool unavailable.
   auto measure_random_latency = [](const char* label,
                                    storage::BufferFrame* pool,
                                    u64 count,
                                    u64 samples) -> double {
      if (!pool || count == 0) return -1.0;

      std::mt19937_64 rng(12345);
      std::uniform_int_distribution<u64> dist(0, count - 1);
      std::vector<u64> indices(samples);
      for (auto& idx : indices) idx = dist(rng);

      // Flush every target cache line so first read misses cache
      print_info(std::string(label) + " flushing " + std::to_string(samples) + " target lines before random access...");
      for (u64 idx : indices) {
         _mm_clflush(&pool[idx].header.pid);
      }
      _mm_mfence();

      volatile u64 sink = 0;
      auto t0 = std::chrono::high_resolution_clock::now();
      for (u64 idx : indices) {
         sink += pool[idx].header.pid;
         _mm_clflush(&pool[idx].header.pid);
         _mm_mfence();
      }
      auto t1 = std::chrono::high_resolution_clock::now();

      double total_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
      double avg_ns   = total_ns / samples;

      std::ostringstream oss;
      oss << std::fixed << std::setprecision(1)
          << label << " random access: " << samples << " samples, "
          << "total=" << total_ns / 1e6 << " ms, "
          << "avg=" << avg_ns << " ns/access (includes clflush overhead)";
      print_info(oss.str());
      (void)sink;
      return avg_ns;
   };

   const u64 rand_samples = 100000;
   double dram_rand_lat = measure_random_latency("DRAM", dram_bfs, dram_num, rand_samples);
   double cxl_rand_lat  = measure_random_latency("CXL ", cxl_bfs,  cxl_num,  rand_samples);

   // --- Step 4: Summary comparison table ---
   auto fmt_bw  = [](double v) -> std::string { if (v < 0) return "N/A"; std::ostringstream o; o << std::fixed << std::setprecision(2) << v; return o.str(); };
   auto fmt_lat = [](double v) -> std::string { if (v < 0) return "N/A"; std::ostringstream o; o << std::fixed << std::setprecision(1) << v; return o.str(); };

   std::cout << "\n";
   std::cout << Color::BOLD
             << std::left  << std::setw(26) << "  Metric"
             << std::right << std::setw(16) << "DRAM"
             << std::right << std::setw(16) << "CXL"
             << std::right << std::setw(12) << "Ratio"
             << Color::RESET << "\n";
   std::cout << "  " << std::string(68, '-') << "\n";

   auto print_row = [](const std::string& label, const std::string& dv, const std::string& cv, const std::string& ratio) {
      std::cout << std::left  << std::setw(26) << ("  " + label)
                << std::right << std::setw(16) << dv
                << std::right << std::setw(16) << cv
                << std::right << std::setw(12) << ratio << "\n";
   };

   auto ratio_str = [](double a, double b) -> std::string {
      if (a <= 0 || b <= 0) return "N/A";
      std::ostringstream o; o << std::fixed << std::setprecision(2) << (a / b) << "x"; return o.str();
   };

   print_row("Seq BW (GiB/s)",  fmt_bw(dram_seq_bw),  fmt_bw(cxl_seq_bw),   ratio_str(dram_seq_bw, cxl_seq_bw));
   print_row("Rand Lat (ns)",   fmt_lat(dram_rand_lat), fmt_lat(cxl_rand_lat), ratio_str(cxl_rand_lat, dram_rand_lat));
   print_row("Active Frames",   std::to_string(dram_active), std::to_string(cxl_active), "");
   print_row("Total Frames",    std::to_string(dram_num), std::to_string(cxl_num), "");
   std::cout << "\n";

   // --- Step 5: Partition stats ---
   print_info("Partition layout: DRAM=" + std::to_string(bm.getDRAMPartitionsCount()) +
              ", CXL=" + std::to_string(bm.getCXLPartitionsCount()) +
              ", total=" + std::to_string(bm.getTotalPartitionsCount()));

   u64 dram_free_total = 0, cxl_free_total = 0;
   auto& parts = bm.getPartitions();
   for (u64 i = 0; i < bm.getDRAMPartitionsCount(); i++) {
      dram_free_total += parts[i]->dram_free_list.counter.load();
   }
   for (u64 i = 0; i < bm.getCXLPartitionsCount(); i++) {
      cxl_free_total += parts[bm.getDRAMPartitionsCount() + i]->cxl_free_list.counter.load();
   }
   print_info("DRAM free BFs: " + std::to_string(dram_free_total) +
              " / " + std::to_string(dram_num) +
              " (free " + std::to_string(100.0 * dram_free_total / dram_num) + "%)");
   if (cxl_num > 0) {
      print_info("CXL  free BFs: " + std::to_string(cxl_free_total) +
                 " / " + std::to_string(cxl_num) +
                 " (free " + std::to_string(100.0 * cxl_free_total / cxl_num) + "%)");
   }
}


// =============================================================================
// Phase 3+4: Lookup with Two-Level Zipfian + Admission Control Monitoring
// =============================================================================
void phase3_lookup_with_monitoring(cr::CRManager& crm, LeanStoreAdapter<KVTable>& table,
                                   const TwoLevelZipfianGenerator& zipf_gen,
                                   u64 total_lookups, u64 warmup_lookups) {
   print_phase("Phase 3+4: Lookup (" + std::to_string(total_lookups) +
               " queries) + Admission Control Monitoring");

   auto& bm = *storage::BMC::global_bf;

   // Reset diagnostic counters
   bm.diag.record_cache_hit.store(0);
   bm.diag.record_cache_miss.store(0);
   bm.diag.dram_buffer_pool_hit.store(0);
   bm.diag.cxl_buffer_pool_hit.store(0);

   // Open CSV output files
   std::ofstream admission_csv(FLAGS_test_admission_log, std::ios::trunc);
   admission_csv << "query_count,threshold_coarse,threshold_fine,num_candidates,global_requests\n";

   std::ofstream hit_csv(FLAGS_test_hit_stats_log, std::ios::trunc);
   hit_csv << "query_count,record_cache_hit,record_cache_miss,dram_buffer_pool_hit,cxl_buffer_pool_hit,record_cache_hit_rate_pct\n";

   const TX_MODE tx_type = TX_MODE::OLTP;
   const u64 print_interval = FLAGS_test_print_interval;

   std::atomic<u64> global_lookup_counter{0};
   std::atomic<u64> abort_counter{0};
   std::atomic<bool> keep_running{true};

   auto run_lookup_batch = [&](u64 batch_lookups, bool count_lookup_counter) {
      const u64 t_cnt = FLAGS_worker_threads;
      const u64 base = batch_lookups / t_cnt;
      const u64 rem = batch_lookups % t_cnt;
      for (u64 t_i = 0; t_i < t_cnt; t_i++) {
         const u64 quota = base + (t_i < rem ? 1 : 0);
         crm.scheduleJobAsync(t_i, [&, t_i, quota]() {
            std::mt19937_64 rng(t_i * 999983ULL + 42);
            u64 completed = 0;
            while (completed < quota) {
               jumpmuTry()
               {
                  TestKey key = zipf_gen.next(rng);
                  cr::Worker::my().startTX(tx_type, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
                  table.lookup1({key}, [&](const KVTable&) {});
                  cr::Worker::my().commitTX();
                  completed++;
                  if (count_lookup_counter) {
                     global_lookup_counter.fetch_add(1, std::memory_order_relaxed);
                  }
               }
               jumpmuCatch()
               {
                  abort_counter.fetch_add(1, std::memory_order_relaxed);
               }
            }
         });
      }
      crm.joinAll();
   };

   if (warmup_lookups > 0) {
      print_info("Warmup begin: " + std::to_string(warmup_lookups) + " lookups (not counted in final hit-rate)");
      run_lookup_batch(warmup_lookups, false);
      bm.diag.record_cache_hit.store(0);
      bm.diag.record_cache_miss.store(0);
      bm.diag.dram_buffer_pool_hit.store(0);
      bm.diag.cxl_buffer_pool_hit.store(0);
      print_info("Warmup complete, counters reset for measured phase.");
   }

   auto t_begin = std::chrono::high_resolution_clock::now();

   // Monitoring thread: polls counters and writes CSV
   std::thread monitor_thread([&]() {
      u64 last_printed = 0;
      while (keep_running.load(std::memory_order_relaxed)) {
         u64 current = global_lookup_counter.load(std::memory_order_relaxed);
         u64 intervals_now = current / print_interval;
         u64 intervals_last = last_printed / print_interval;

         if (intervals_now > intervals_last) {
            last_printed = intervals_now * print_interval;

            // Hit-rate stats
            u64 rc_h = bm.diag.record_cache_hit.load(std::memory_order_relaxed);
            u64 rc_m = bm.diag.record_cache_miss.load(std::memory_order_relaxed);
            u64 dram_h = bm.diag.dram_buffer_pool_hit.load(std::memory_order_relaxed);
            u64 cxl_h = bm.diag.cxl_buffer_pool_hit.load(std::memory_order_relaxed);
            double rc_pct = (rc_h + rc_m > 0) ? (100.0 * rc_h / (rc_h + rc_m)) : 0;

            hit_csv << last_printed << "," << rc_h << "," << rc_m << ","
                    << dram_h << "," << cxl_h << ","
                    << std::fixed << std::setprecision(4) << rc_pct << "\n";

            // Admission control stats
            auto* wrapper = bm.getAdmissionControlWrapper();
            if (wrapper) {
               auto& hist = wrapper->GetHistogram();
               auto& candidates = wrapper->GetHotPageCandidates();

               u64 th_coarse = hist.GetAdmissionThreshold_coarse();
               u64 th_fine = hist.GetAdmissionThreshold_fine();
               u64 num_cand = candidates.GetCandidatesSize();
               u64 g_req = candidates.GetGlobalRequests();

               admission_csv << last_printed << "," << th_coarse << "," << th_fine << ","
                             << num_cand << "," << g_req << "\n";
            }

            // Terminal summary every 1M lookups
            if (last_printed % 1000000 == 0) {
               auto t_now = std::chrono::high_resolution_clock::now();
               double elapsed = std::chrono::duration<double>(t_now - t_begin).count();
               double mtps = last_printed / elapsed / 1e6;
               std::ostringstream oss;
               oss << std::fixed << std::setprecision(2)
                   << "[" << last_printed / 1000000 << "M] "
                   << mtps << " Mtps | record_cache_hit=" << bm.diag.record_cache_hit.load()
                   << " DRAM Buffer Pool hit=" << bm.diag.dram_buffer_pool_hit.load()
                   << " CXL Buffer Pool hit=" << bm.diag.cxl_buffer_pool_hit.load();
               if (wrapper) {
                  oss << " | threshold=" << wrapper->GetHistogram().GetAdmissionThreshold_fine()
                      << " candidates=" << wrapper->GetHotPageCandidates().GetCandidatesSize();
               }
               print_info(oss.str());
            }
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
   });

   // Worker threads execute measured lookups
   run_lookup_batch(total_lookups, true);
   keep_running.store(false);
   monitor_thread.join();

   auto t_end = std::chrono::high_resolution_clock::now();
   double total_secs = std::chrono::duration<double>(t_end - t_begin).count();
   u64 actual_lookups = global_lookup_counter.load();

   admission_csv.close();
   hit_csv.close();

   // Final summary
   u64 rc_h = bm.diag.record_cache_hit.load();
   u64 rc_m = bm.diag.record_cache_miss.load();
   u64 dram_h = bm.diag.dram_buffer_pool_hit.load();
   u64 cxl_h = bm.diag.cxl_buffer_pool_hit.load();

   print_info("Lookup complete: " + std::to_string(actual_lookups) + " queries in "
              + std::to_string(total_secs) + " sec ("
              + std::to_string(actual_lookups / total_secs / 1e6) + " Mtps)");
   print_info("RecordCache hits:  " + std::to_string(rc_h)
              + " (" + std::to_string(rc_h + rc_m > 0 ? 100.0 * rc_h / (rc_h + rc_m) : 0) + "%)");
   print_info("DRAM BTree hits:   " + std::to_string(dram_h));
   print_info("CXL  BTree hits:   " + std::to_string(cxl_h));
   print_info("TX aborts:         " + std::to_string(abort_counter.load()));
   print_info("Admission log -> " + FLAGS_test_admission_log);
   print_info("Hit stats log  -> " + FLAGS_test_hit_stats_log);

   // Print final histogram if available
   auto* wrapper = bm.getAdmissionControlWrapper();
   if (wrapper) {
      std::string hist_str = wrapper->GetHistogram().PrintHistogram();
      std::cout << hist_str << std::endl;
   }
}

// =============================================================================
// Phase 5: Partition Debug Dump
// =============================================================================
void phase5_partition_dump() {
   print_phase("Phase 5: Partition Debug Dump");

   auto& bm = *storage::BMC::global_bf;
   storage::BufferFrame* dram_bfs = bm.getDRAMBFs();
   storage::BufferFrame* cxl_bfs  = bm.getCXLBFs();
   u64 dram_num = bm.getPoolSize();
   u64 cxl_num  = bm.getCXLPoolSize();
   u64 dram_parts = bm.getDRAMPartitionsCount();
   u64 cxl_parts  = bm.getCXLPartitionsCount();
   auto& parts = bm.getPartitions();

   // Helper: state to string
   auto state_str = [](storage::BufferFrame::STATE s) -> const char* {
      switch (s) {
         case storage::BufferFrame::STATE::FREE: return "FREE";
         case storage::BufferFrame::STATE::HOT:  return "HOT";
         case storage::BufferFrame::STATE::COOL: return "COOL";
         case storage::BufferFrame::STATE::LOADED: return "LOADED";
         default: return "???";
      }
   };

   // Check tier isolation: no DRAM BF should be in CXL free lists, and vice versa
   // (We can't directly check free-list membership, but we check if any active
   //  DRAM BF has a PID that routes to a CXL partition or vice versa)

   // Dump selected DRAM partitions
   auto dump_dram_partition = [&](u64 p_i) {
      if (p_i >= dram_parts) return;
      u64 free_count = parts[p_i]->dram_free_list.counter.load();
      u64 cxl_fl = parts[p_i]->cxl_free_list.counter.load();

      std::cout << "\n=== DRAM Partition " << p_i << " ===" << std::endl;
      std::cout << "  dram_free_list.counter = " << free_count << std::endl;
      std::cout << "  cxl_free_list.counter  = " << cxl_fl;
      if (cxl_fl > 0) {
         std::cout << " [WARNING: should be 0 for DRAM partition!]";
      }
      std::cout << std::endl;

      u64 active_count = 0;
      u64 sample_limit = 5;
      std::cout << "  Sample active BFs (pid routes to this partition):" << std::endl;
      for (u64 bf_i = 0; bf_i < dram_num && sample_limit > 0; bf_i++) {
         auto& bf = dram_bfs[bf_i];
         if (bf.header.state != storage::BufferFrame::STATE::FREE) {
            u64 routed = bf.header.pid & (dram_parts - 1);
            if (routed == p_i) {
               active_count++;
               if (sample_limit > 0) {
                  std::cout << "    bf_idx=" << bf_i
                            << " pid=" << bf.header.pid
                            << " state=" << state_str(bf.header.state)
                            << " is_dirty=" << bf.isDirty() << std::endl;
                  sample_limit--;
               }
            }
         }
      }
      std::cout << "  Total BFs with pid routed here: " << active_count << std::endl;
   };

   auto dump_cxl_partition = [&](u64 p_i) {
      u64 abs_i = dram_parts + p_i;
      if (abs_i >= parts.size()) return;
      u64 cxl_fl = parts[abs_i]->cxl_free_list.counter.load();
      u64 dram_fl = parts[abs_i]->dram_free_list.counter.load();

      std::cout << "\n=== CXL Partition " << p_i << " (index " << abs_i << ") ===" << std::endl;
      std::cout << "  cxl_free_list.counter  = " << cxl_fl << std::endl;
      std::cout << "  dram_free_list.counter = " << dram_fl;
      if (dram_fl > 0) {
         std::cout << " [WARNING: should be 0 for CXL partition!]";
      }
      std::cout << std::endl;

      if (cxl_bfs && cxl_num > 0) {
         u64 active_count = 0;
         u64 sample_limit = 5;
         std::cout << "  Sample active CXL BFs:" << std::endl;
         for (u64 bf_i = 0; bf_i < cxl_num && sample_limit > 0; bf_i++) {
            auto& bf = cxl_bfs[bf_i];
            if (bf.header.state != storage::BufferFrame::STATE::FREE) {
               u64 routed = bf_i % cxl_parts;
               if (routed == p_i) {
                  active_count++;
                  if (sample_limit > 0) {
                     std::cout << "    cxl_bf_idx=" << bf_i
                               << " pid=" << bf.header.pid
                               << " state=" << state_str(bf.header.state)
                               << " is_dirty=" << bf.isDirty() << std::endl;
                     sample_limit--;
                  }
               }
            }
         }
         std::cout << "  Total CXL BFs routed here: " << active_count << std::endl;
      }
   };

   // Dump a representative set of partitions
   print_info("Dumping representative DRAM partitions: 0, 1, " +
              std::to_string(dram_parts - 1));
   dump_dram_partition(0);
   dump_dram_partition(1);
   dump_dram_partition(dram_parts - 1);

   if (cxl_parts > 0) {
      print_info("Dumping representative CXL partitions: 0, 1, " +
                 std::to_string(cxl_parts - 1));
      dump_cxl_partition(0);
      dump_cxl_partition(1);
      dump_cxl_partition(cxl_parts - 1);
   }

   // Tier isolation sanity check
   print_info("Running tier isolation sanity check...");
   bool isolation_ok = true;
   for (u64 i = 0; i < dram_parts; i++) {
      if (parts[i]->cxl_free_list.counter.load() > 0) {
         print_fail("DRAM Partition " + std::to_string(i) +
                    " has non-zero cxl_free_list (" +
                    std::to_string(parts[i]->cxl_free_list.counter.load()) + ")");
         isolation_ok = false;
      }
   }
   for (u64 i = 0; i < cxl_parts; i++) {
      u64 abs_i = dram_parts + i;
      if (parts[abs_i]->dram_free_list.counter.load() > 0) {
         print_fail("CXL Partition " + std::to_string(i) +
                    " has non-zero dram_free_list (" +
                    std::to_string(parts[abs_i]->dram_free_list.counter.load()) + ")");
         isolation_ok = false;
      }
   }
   if (isolation_ok) {
      print_pass("Tier isolation check passed: no cross-tier free-list contamination");
   }
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv)
{
   gflags::SetUsageMessage("CXL Lookup Integration Test");
   gflags::ParseCommandLineFlags(&argc, &argv, true);
   // -------------------------------------------------------------------------------------
   std::cout << "\n" << Color::BOLD << Color::CYAN
             << "============================================\n"
             << "  CXL Lookup Integration Test\n"
             << "============================================"
             << Color::RESET << "\n";

   print_info("Configuration:");
   print_info("  cxl_tiering_enabled  = " + std::to_string(FLAGS_cxl_tiering_enabled));
   print_info("  dram_buffer_pool_gib = " + std::to_string(FLAGS_dram_buffer_pool_gib));
   print_info("  dram_recordcache_gib = " + std::to_string(FLAGS_dram_recordcache_gib));
   print_info("  cxl_gib              = " + std::to_string(FLAGS_cxl_gib));
   print_info("  cxl_dax_device_path  = " + FLAGS_cxl_dax_device_path);
   print_info("  vi (BTreeVI)         = " + std::to_string(FLAGS_vi));
   print_info("  worker_threads       = " + std::to_string(FLAGS_worker_threads));
   print_info("  test_load_gib        = " + std::to_string(FLAGS_test_load_gib));
   print_info("  test_data_gib        = " + std::to_string(FLAGS_test_data_gib));
   print_info("  test_theta_page      = " + std::to_string(FLAGS_test_theta_page));
   print_info("  test_theta_slot      = " + std::to_string(FLAGS_test_theta_slot));
   print_info("  pp_threads           = " + std::to_string(FLAGS_pp_threads));
   print_info("  cxl_pp_threads       = " + std::to_string(FLAGS_cxl_pp_threads));
   print_info("  two_level_admission  = " + std::to_string(FLAGS_two_level_admission_threads));
   print_info("  forward_epoch_thread = " + std::to_string(FLAGS_forward_epoch_thread));
   print_info("  sieve_eviction_thread= " + std::to_string(FLAGS_sieve_eviction_thread));
   print_info("  rc_promote_thread    = " + std::to_string(FLAGS_record_cache_promote_thread));
   print_info("  test_total_lookups   = " + std::to_string(FLAGS_test_total_lookups));
   print_info("  test_warmup_lookups  = " + std::to_string(FLAGS_test_warmup_lookups));
   print_info("  test_print_interval  = " + std::to_string(FLAGS_test_print_interval));

   if (!FLAGS_cxl_tiering_enabled) {
      print_fail("This test requires --cxl_tiering_enabled=true");
      return 1;
   }

   // -------------------------------------------------------------------------------------
   // Compute tuple count
   const u64 records_per_page = FLAGS_test_records_per_page;
   // Backward compatible:
   // - prefer --test_load_gib when set (>0)
   // - otherwise fall back to legacy --test_data_gib
   const double load_gib = (FLAGS_test_load_gib > 0.0) ? FLAGS_test_load_gib : FLAGS_test_data_gib;
   if (load_gib <= 0.0 && FLAGS_test_tuple_count == 0) {
      print_fail("Invalid load size. Set --test_load_gib>0 (or --test_data_gib>0), "
                 "or provide --test_tuple_count directly.");
      return 1;
   }
   const u64 total_pages = static_cast<u64>(load_gib * 1024 * 1024 * 1024 / storage::PAGE_SIZE);
   const u64 tuple_count = FLAGS_test_tuple_count ? FLAGS_test_tuple_count : total_pages * records_per_page;
   const u64 num_pages = tuple_count / records_per_page;

   print_info("  Computed: tuple_count=" + std::to_string(tuple_count) +
              ", est_pages=" + std::to_string(num_pages) +
              ", records_per_page=" + std::to_string(records_per_page) +
              ", effective_load_gib=" + std::to_string(load_gib));
   print_theory_bounds(num_pages, records_per_page, FLAGS_test_theta_page);

   // -------------------------------------------------------------------------------------
   // Build Two-Level Zipfian generator (before LeanStore init so we don't delay SSD open)
   TwoLevelZipfianGenerator zipf_gen;
   zipf_gen.init(num_pages, records_per_page, FLAGS_test_theta_page, FLAGS_test_theta_slot);

   // -------------------------------------------------------------------------------------
   // Initialize LeanStore
   print_phase("Initializing LeanStore");
   LeanStore db;
   auto& crm = db.getCRManager();

   // Register table on worker 0
   LeanStoreAdapter<KVTable> table;
   crm.scheduleJobSync(0, [&]() {
      table = LeanStoreAdapter<KVTable>(db, "CXL_TEST");
   });

   print_pass(std::string("LeanStore initialized, ") + (FLAGS_vi ? "BTreeVI" : "BTreeLL") + " registered");

   // -------------------------------------------------------------------------------------
   // Run test phases
   try {
      // Phase 1: Load data
      phase1_load_data(crm, table, tuple_count);

      // Phase 2: CXL verification + bandwidth
      phase2_cxl_verification_and_bandwidth();

      // Phase 3+4: Lookup with monitoring + hit stats
      phase3_lookup_with_monitoring(crm, table, zipf_gen, FLAGS_test_total_lookups, FLAGS_test_warmup_lookups);

      // Phase 5: Partition debug dump
      phase5_partition_dump();

   } catch (const std::exception& e) {
      print_fail(std::string("Exception: ") + e.what());
      return 1;
   }

   // -------------------------------------------------------------------------------------
   std::cout << "\n" << Color::BOLD << Color::GREEN
             << "============================================\n"
             << "  CXL Lookup Integration Test COMPLETE\n"
             << "============================================"
             << Color::RESET << "\n";
   return 0;
}
