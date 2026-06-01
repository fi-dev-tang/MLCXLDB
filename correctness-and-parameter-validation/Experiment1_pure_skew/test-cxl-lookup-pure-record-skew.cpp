// =============================================================================
// test-cxl-lookup-pure-record-skew.cpp
//
// Correctness-focused CXL lookup test using the exact same record generator
// logic as Experiment1_pure_skew/pure_record_cache_data_generator.cpp:
//
//   - SplitMix64 RNG
//   - ZipfGenerator
//   - fnvHash64 scrambling
//   - ScrambledZipfGenerator (record_id stream)
//
// Goals:
//   1) Verify RecordCache correctness under pure record skew
//   2) Ensure lookup key space matches loaded key space
//   3) Measure RecordCache / DRAM / CXL hit statistics
//   4) Provide a workload sanity report
//
// This test keeps the same integration style as
// test-cxl-lookup-integration.cpp, but replaces the lookup generator
// with a pure-record-skew generator.
//
// =============================================================================
// Build:  cmake target "cxl_lookup_pure_record_skew"
// Run:    
/*
(two_level): full

./build/frontend/cxl_lookup_pure_record_skew_test \
  --test_admission_mode=two_level \
  --cxl_tiering_enabled=true \
  --dram_buffer_pool_gib=0.25 \
  --dram_recordcache_gib=1 \
  --cxl_gib=16 \
  --cxl_dax_device_path=/dev/dax0.2 \
  --worker_threads=4 \
  --vi=true \
  --pp_threads=1 --cxl_pp_threads=1 \
  --two_level_admission_threads=1 \
  --forward_epoch_thread=1 --sieve_eviction_thread=1 \
  --record_cache_promote_thread=1 \
  --test_progress_interval=1000000 \
  --ssd_path=/home/zhizhi.tyf/cxl-recordcache-dev/tmp/cxl_test_ssd --trunc=true --wal=true \
  --delay_admission_recordcache_threads_start=true \
  2>&1 | tee "result_lookup_pure_fullsystem_$(date +%Y%m%d_%H%M%S).csv"

(PageOnly): first-level
./build/frontend/cxl_lookup_pure_record_skew_test \
  --test_admission_mode=page_only \
  --cxl_tiering_enabled=true \
  --dram_buffer_pool_gib=1.25 \
  --cxl_gib=16 \
  --cxl_dax_device_path=/dev/dax0.2 \
  --worker_threads=4 \
  --vi=true \
  --pp_threads=1 --cxl_pp_threads=1 \
  --test_progress_interval=1000000 \
  --two_level_admission_threads=1 \
  --ssd_path=/home/zhizhi.tyf/cxl-recordcache-dev/tmp/cxl_test_ssd --trunc=true --wal=true \
  --delay_admission_recordcache_threads_start=true \
  2>&1 | tee "result_lookup_pure_pageOnly_$(date +%Y%m%d_%H%M%S).csv"

(LRU): baseline
./build/frontend/cxl_lookup_pure_record_skew_test \
  --test_admission_mode=lru \
  --cxl_tiering_enabled=true \
  --dram_buffer_pool_gib=1.25 \
  --cxl_gib=16 \
  --cxl_dax_device_path=/dev/dax0.2 \
  --worker_threads=4 \
  --vi=true \
  --pp_threads=1 --cxl_pp_threads=1 \
  --two_level_admission_threads=1 \
  --test_progress_interval=1000000 \
  --ssd_path=/home/zhizhi.tyf/cxl-recordcache-dev/tmp/cxl_test_ssd --trunc=true --wal=true \
  --delay_admission_recordcache_threads_start=true \
  2>&1 | tee "result_lookup_pure_baseline_$(date +%Y%m%d_%H%M%S).csv"
*/

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

#include <gflags/gflags.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <mutex>
#include <vector>

using namespace leanstore;
using u64 = std::uint64_t;

using TestKey = u64;
using TestPayload = BytesPayload<8>;
using KVTable = Relation<TestKey, TestPayload>;

constexpr u64 GiB = 1024ULL * 1024ULL * 1024ULL;

// ============================================================================
// gflags
// ============================================================================
DEFINE_uint64(test_tuple_count, 0, "Total records to insert (0 = auto from working set)");
DEFINE_double(test_working_set_gib, 4.0, "Target working set size in GiB");
DEFINE_double(test_record_cache_gib, 1.0, "RecordCache size in GiB");
DEFINE_uint64(test_record_cache_entry_size, 24, "Bytes per cached record entry");
DEFINE_double(test_zipf_theta, 0.99, "Zipfian skew parameter");
DEFINE_uint64(test_record_size_bytes, 16, "Bytes per record in key space estimation");
DEFINE_uint64(test_page_size_bytes, 16384, "Page size in bytes");
DEFINE_double(test_fill_factor, 0.5, "B+Tree fill factor");
DEFINE_uint64(test_warmup_lookups, 20000000ULL, "Warmup lookup count before measured phase");
DEFINE_uint64(test_measure_lookups, 100000000ULL, "Measured lookup count");
DEFINE_uint64(test_progress_interval, 10000ULL, "Print progress every N measured lookups (default 1w)");
DEFINE_uint64(test_seed, 42ULL, "Random seed");
DEFINE_double(test_skew_top1_threshold, 0.10, "Threshold for identifying skew page in sanity report");
DEFINE_uint64(test_sample_count, 10000000ULL, "Sample count for workload sanity report");
DEFINE_string(test_admission_log, "admission_control_log.csv", "Output file for admission stats");
DEFINE_string(test_hit_stats_log, "hit_stats.csv", "Output file for hit-rate stats");
DEFINE_string(test_admission_mode, "two_level", "Ablation admission mode: lru, page_only, two_level");
DEFINE_uint64(test_latency_sample_rate, 100, "Record one latency sample every N successful lookups during measured phase (1 = all)");

// ============================================================================
// Color helpers
// ============================================================================
namespace Color {
const char* RESET   = "\033[0m";
const char* RED     = "\033[31m";
const char* GREEN   = "\033[32m";
const char* YELLOW  = "\033[33m";
const char* CYAN    = "\033[36m";
const char* MAGENTA = "\033[35m";
const char* BOLD    = "\033[1m";
} // namespace Color

void print_info(const std::string& msg) { std::cout << Color::CYAN << "[INFO] " << Color::RESET << msg << "\n"; }
void print_warn(const std::string& msg) { std::cout << Color::YELLOW << "[WARN] " << Color::RESET << msg << "\n"; }
void print_pass(const std::string& msg) { std::cout << Color::GREEN << "[PASS] " << Color::RESET << msg << "\n"; }
void print_fail(const std::string& msg) { std::cout << Color::RED << "[FAIL] " << Color::RESET << msg << "\n"; }
void print_phase(const std::string& msg) {
   std::cout << "\n" << Color::BOLD << Color::MAGENTA
             << "========================================\n"
             << "  " << msg << "\n"
             << "========================================" << Color::RESET << "\n";
}

// ============================================================================
// Config
// ============================================================================
struct Config {
   double   fill_factor             = 0.5;
   double   working_set_gib         = 4.0;
   double   record_cache_gib        = 1.0;
   u64      record_cache_entry_size  = 24;
   double   zipf_theta              = 0.99;
   u64      record_size_bytes       = 16;
   u64      page_size_bytes         = 16384;
   u64      sample_count            = 10000000ULL;
   u64      seed                    = 42ULL;
   double   skew_top1_threshold     = 0.10;
   std::string admission_mode       = "two_level";

   u64 records_per_page() const {
      const u64 per_page = static_cast<u64>(page_size_bytes / record_size_bytes);
      return static_cast<u64>(per_page * fill_factor);
   }

   u64 total_records() const {
      return static_cast<u64>(working_set_gib * GiB) / record_size_bytes;
   }

   u64 total_pages() const {
      const u64 rpp = records_per_page();
      return (total_records() + rpp - 1) / rpp;
   }

   u64 record_cache_capacity() const {
      return static_cast<u64>(record_cache_gib * GiB) / record_cache_entry_size;
   }
};

Config build_config_from_flags() {
   Config cfg;
   cfg.fill_factor            = FLAGS_test_fill_factor;
   cfg.working_set_gib        = FLAGS_test_working_set_gib;
   cfg.record_cache_gib       = FLAGS_test_record_cache_gib;
   cfg.record_cache_entry_size = FLAGS_test_record_cache_entry_size;
   cfg.zipf_theta             = FLAGS_test_zipf_theta;
   cfg.record_size_bytes      = FLAGS_test_record_size_bytes;
   cfg.page_size_bytes        = FLAGS_test_page_size_bytes;
   cfg.sample_count           = FLAGS_test_sample_count;
   cfg.seed                   = FLAGS_test_seed;
   cfg.skew_top1_threshold    = FLAGS_test_skew_top1_threshold;
   cfg.admission_mode         = FLAGS_test_admission_mode;
   return cfg;
}

// ============================================================================
// SplitMix64
// ============================================================================
class SplitMix64 {
public:
   explicit SplitMix64(u64 seed) : state_(seed) {}

   u64 next() {
      u64 z = (state_ += 0x9e3779b97f4a7c15ULL);
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
      return z ^ (z >> 31);
   }

   double next01() {
      return static_cast<double>(next()) / static_cast<double>(std::numeric_limits<u64>::max());
   }

private:
   u64 state_;
};

// ============================================================================
// FNV-1a 64-bit hash for scrambling
// ============================================================================
u64 fnvHash64(u64 val) {
   constexpr u64 FNV_OFFSET = 0xCBF29CE484222325ULL;
   constexpr u64 FNV_PRIME  = 1099511628211ULL;
   u64 h = FNV_OFFSET;
   for (int i = 0; i < 8; i++) {
      h ^= (val & 0xffULL);
      h *= FNV_PRIME;
      val >>= 8;
   }
   return h;
}

// ============================================================================
// Zipf Generator (rank in [1, n])
// ============================================================================
class ZipfGenerator {
public:
   ZipfGenerator() = default;
   ZipfGenerator(u64 n, double theta) { reset(n, theta); }

   void reset(u64 n, double theta) {
      if (n < 2) throw std::invalid_argument("ZipfGenerator: n must be >= 2");
      n_ = n;
      theta_ = theta;
      alpha_ = 1.0 / (1.0 - theta);
      zetan_ = zeta(n, theta);
      eta_ = (1.0 - std::pow(2.0 / n, 1.0 - theta)) /
             (1.0 - zeta(2, theta) / zetan_);
   }

   u64 next(SplitMix64& rng) const {
      const double u = rng.next01();
      const double uz = u * zetan_;
      if (uz < 1.0) return 1;
      if (uz < (1.0 + std::pow(0.5, theta_))) return 2;
      return 1 + static_cast<u64>(static_cast<double>(n_) * std::pow(eta_ * u - eta_ + 1.0, alpha_));
   }

private:
   static double zeta(u64 n, double theta) {
      double s = 0.0;
      for (u64 i = 1; i <= n; i++) s += std::pow(1.0 / i, theta);
      return s;
   }

   u64 n_ = 0;
   double theta_ = 0.0;
   double alpha_ = 0.0;
   double zetan_ = 0.0;
   double eta_ = 0.0;
};

// ============================================================================
// ScrambledZipfGenerator
//   - zipf rank -> FNV scramble -> [0, n)
// ============================================================================
class ScrambledZipfGenerator {
public:
   ScrambledZipfGenerator() = default;
   ScrambledZipfGenerator(u64 n, double theta) { reset(n, theta); }

   void reset(u64 n, double theta) {
      n_ = n;
      zipf_.reset(n, theta);
   }

   u64 next(SplitMix64& rng) const {
      return fnvHash64(zipf_.next(rng)) % n_;
   }

   u64 size() const { return n_; }

private:
   u64 n_ = 0;
   ZipfGenerator zipf_;
};

// ============================================================================
// Theoretical top-K hit ratio under pure Zipf
// ============================================================================
double theoretical_topk_hit_ratio(u64 total_records, u64 rc_capacity, double theta) {
   const u64 K = std::min(rc_capacity, total_records);
   const u64 exact_limit = std::min(total_records, u64{20000000});

   double zeta_n = 0.0;
   double zeta_k = 0.0;

   for (u64 i = 1; i <= exact_limit; i++) {
      const double term = std::pow(1.0 / i, theta);
      zeta_n += term;
      if (i <= K) zeta_k += term;
   }

   if (total_records > exact_limit) {
      const double L = static_cast<double>(exact_limit);
      const double N = static_cast<double>(total_records);
      const double exp = 1.0 - theta;
      const double tail = (std::pow(N + 0.5, exp) - std::pow(L + 0.5, exp)) / exp;
      zeta_n += tail;

      if (K > exact_limit) {
         const double Kd = static_cast<double>(K);
         const double tail_k = (std::pow(Kd + 0.5, exp) - std::pow(L + 0.5, exp)) / exp;
         zeta_k += tail_k;
      }
   }

   return (zeta_n > 0.0) ? (zeta_k / zeta_n) : 0.0;
}

// ============================================================================
// Workload sanity report
//   This does not prove the system behavior, but confirms that the workload
//   does look like "pure record skew" in the intended key space.
// ============================================================================
void print_workload_sanity_report(const Config& cfg) {
   const u64 total_records = cfg.total_records();
   const u64 total_pages = cfg.total_pages();
   const u64 rc_capacity = cfg.record_cache_capacity();
   const double topk_hr = theoretical_topk_hit_ratio(total_records, rc_capacity, cfg.zipf_theta) * 100.0;

   print_phase("Workload Sanity Report");
   print_info("working_set_gib         = " + std::to_string(cfg.working_set_gib));
   print_info("total_records           = " + std::to_string(total_records));
   print_info("records_per_page        = " + std::to_string(cfg.records_per_page()));
   print_info("total_pages             = " + std::to_string(total_pages));
   print_info("record_cache_gib        = " + std::to_string(cfg.record_cache_gib));
   print_info("record_cache_capacity   = " + std::to_string(rc_capacity));
   print_info("zipf_theta              = " + std::to_string(cfg.zipf_theta));
   print_info("theoretical_topK_HR     = " + std::to_string(topk_hr) + "%");
   print_info("seed                    = " + std::to_string(cfg.seed));
}

struct LookupRunStats {
   double secs = 0.0;
   u64 lookups = 0;
   u64 aborts = 0;
   u64 found = 0;
   u64 not_found = 0;
   double qps = 0.0;
   double tps = 0.0;
   double avg_latency_us = 0.0;
   double p95_latency_us = 0.0;
   double p99_latency_us = 0.0;
   u64 latency_samples = 0;
   u64 record_cache_hit = 0;
   u64 record_cache_miss = 0;
   u64 dram_hit = 0;
   u64 cxl_hit = 0;
   u64 cxl_to_dram_promotions = 0;
   u64 evictions = 0;
};

static double percentile_from_sorted(const std::vector<u64>& sorted, double pct) {
   if (sorted.empty()) return 0.0;
   const double pos = (pct / 100.0) * static_cast<double>(sorted.size() - 1);
   const size_t idx = static_cast<size_t>(pos);
   return static_cast<double>(sorted[idx]);
}

// ============================================================================
// Load data: insert sequential key space [0, total_records)
// ============================================================================
void phase1_load_data(cr::CRManager& crm, LeanStoreAdapter<KVTable>& table, u64 tuple_count) {
   print_phase("Phase 1: Load Data");

   const TX_MODE tx_type = TX_MODE::OLTP;
   auto t0 = std::chrono::high_resolution_clock::now();

   utils::Parallelize::range(FLAGS_worker_threads, tuple_count, [&](u64 t_i, u64 begin, u64 end) {
      crm.scheduleJobAsync(t_i, [&, begin, end]() {
         for (u64 i = begin; i < end; i++) {
            TestPayload payload;
            utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(TestPayload));

            cr::Worker::my().startTX(tx_type, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
            table.insert({i}, {payload});
            cr::Worker::my().commitTX();
         }
      });
   });

   crm.joinAll();

   auto t1 = std::chrono::high_resolution_clock::now();
   double secs = std::chrono::duration<double>(t1 - t0).count();
   print_info("Loaded records: " + std::to_string(tuple_count) +
              ", time: " + std::to_string(secs) + " s");
}

// ============================================================================
// Lookup phase: pure record skew
//   - warmup phase
//   - measured phase
//   - collect RC/DRAM/CXL stats
//   - collect found/not-found stats
// ============================================================================
LookupRunStats run_lookup_phase(cr::CRManager& crm,
                                LeanStoreAdapter<KVTable>& table,
                                const Config& cfg,
                                const ScrambledZipfGenerator& generator,
                                u64 lookup_count,
                                bool measured) {
   auto& bm = *storage::BMC::global_bf;
   const TX_MODE tx_type = TX_MODE::OLTP;

   std::atomic<u64> done{0};
   std::atomic<u64> aborts{0};
   std::atomic<u64> founds{0};
   std::atomic<u64> not_founds{0};
   std::mutex lat_mutex;
   std::vector<u64> latency_samples_us;
   latency_samples_us.reserve(std::min<u64>(lookup_count, 2000000ULL));

   auto t0 = std::chrono::high_resolution_clock::now();

   const u64 t_cnt = FLAGS_worker_threads;
   const u64 base = lookup_count / t_cnt;
   const u64 rem = lookup_count % t_cnt;

   for (u64 t_i = 0; t_i < t_cnt; t_i++) {
      const u64 quota = base + (t_i < rem ? 1 : 0);
      crm.scheduleJobAsync(t_i, [&, t_i, quota]() {
         SplitMix64 rng(cfg.seed + 0x9e3779b97f4a7c15ULL * (t_i + 1));
         std::vector<u64> local_latency_samples;
         if (measured) {
            local_latency_samples.reserve(std::min<u64>(quota, 100000ULL));
         }

         u64 local_done = 0;
         while (local_done < quota) {
            jumpmuTry()
            {
               const auto t_lookup_begin = measured ? std::chrono::high_resolution_clock::now()
                                                    : std::chrono::high_resolution_clock::time_point{};
               const TestKey key = generator.next(rng);
               bool found = false;

               cr::Worker::my().startTX(tx_type, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
               table.lookup1({key}, [&](const KVTable&) {
                  found = true;
               });
               cr::Worker::my().commitTX();

               if (found) founds.fetch_add(1, std::memory_order_relaxed);
               else not_founds.fetch_add(1, std::memory_order_relaxed);

               local_done++;

               if (measured && FLAGS_test_latency_sample_rate > 0 &&
                   (local_done % FLAGS_test_latency_sample_rate == 0)) {
                  const auto t_lookup_end = std::chrono::high_resolution_clock::now();
                  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t_lookup_end - t_lookup_begin).count();
                  local_latency_samples.push_back(static_cast<u64>(us));
               }

               const u64 g = done.fetch_add(1, std::memory_order_relaxed) + 1;
               if (measured && FLAGS_test_progress_interval > 0 &&
                   g % FLAGS_test_progress_interval == 0) {
                  const u64 rc_h = bm.diag.record_cache_hit.load(std::memory_order_relaxed);
                  const u64 rc_m = bm.diag.record_cache_miss.load(std::memory_order_relaxed);
                  const double rc_hr = (rc_h + rc_m > 0) ? (100.0 * rc_h / (rc_h + rc_m)) : 0.0;
                  const u64 dram_h = bm.diag.dram_buffer_pool_hit.load(std::memory_order_relaxed);
                  const u64 cxl_h = bm.diag.cxl_buffer_pool_hit.load(std::memory_order_relaxed);
                  const double dram_hr_per_lookup = (g > 0) ? (100.0 * static_cast<double>(dram_h) / static_cast<double>(g)) : 0.0;
                  const double cxl_hr_per_lookup = (g > 0) ? (100.0 * static_cast<double>(cxl_h) / static_cast<double>(g)) : 0.0;
                  const u64 tier_hits = dram_h + cxl_h;
                  const double dram_hr_within_tier = (tier_hits > 0) ? (100.0 * static_cast<double>(dram_h) / static_cast<double>(tier_hits)) : 0.0;
                  const double cxl_hr_within_tier = (tier_hits > 0) ? (100.0 * static_cast<double>(cxl_h) / static_cast<double>(tier_hits)) : 0.0;

                  const bool is_two_level = (FLAGS_admission_mode == "two_level");
                  u64 fine_grained_threshold = 0;
                  u64 dram_hot_candidates = 0;
                  if (auto* wrapper = bm.getAdmissionControlWrapper();
                      is_two_level && wrapper != nullptr) {
                     fine_grained_threshold = wrapper->GetHistogram().GetAdmissionThreshold_fine();
                     dram_hot_candidates = wrapper->GetHotPageCandidates().GetCandidatesSize();
                  }

                  if (is_two_level) {
                     print_info("Progress " + std::to_string(g) + "/" + std::to_string(lookup_count) +
                                ", RC HR=" + std::to_string(rc_hr) + "%" +
                                ", DRAM HR(per_lookup)=" + std::to_string(dram_hr_per_lookup) + "%" +
                                ", CXL HR(per_lookup)=" + std::to_string(cxl_hr_per_lookup) + "%" +
                                ", fine_threshold=" + std::to_string(fine_grained_threshold) +
                                ", dram_hot_candidates=" + std::to_string(dram_hot_candidates));
                  } else {
                     print_info("Progress " + std::to_string(g) + "/" + std::to_string(lookup_count) +
                                ", DRAM hit=" + std::to_string(dram_h) +
                                " (" + std::to_string(dram_hr_per_lookup) + "% per_lookup, " +
                                std::to_string(dram_hr_within_tier) + "% within_tier)" +
                                ", CXL hit=" + std::to_string(cxl_h) +
                                " (" + std::to_string(cxl_hr_per_lookup) + "% per_lookup, " +
                                std::to_string(cxl_hr_within_tier) + "% within_tier)");
                  }
               }
            }
            jumpmuCatch()
            {
               aborts.fetch_add(1, std::memory_order_relaxed);
            }
         }
         if (measured && !local_latency_samples.empty()) {
            std::lock_guard<std::mutex> lock(lat_mutex);
            latency_samples_us.insert(latency_samples_us.end(), local_latency_samples.begin(), local_latency_samples.end());
         }
      });
   }

   crm.joinAll();

   auto t1 = std::chrono::high_resolution_clock::now();
   double secs = std::chrono::duration<double>(t1 - t0).count();
   LookupRunStats stats;
   stats.secs = secs;
   stats.lookups = lookup_count;
   stats.aborts = aborts.load(std::memory_order_relaxed);
   stats.found = founds.load(std::memory_order_relaxed);
   stats.not_found = not_founds.load(std::memory_order_relaxed);
   stats.qps = (secs > 0.0) ? (static_cast<double>(lookup_count) / secs) : 0.0;
   stats.tps = stats.qps;

   if (measured) {
      const u64 rc_h = bm.diag.record_cache_hit.load(std::memory_order_relaxed);
      const u64 rc_m = bm.diag.record_cache_miss.load(std::memory_order_relaxed);
      const u64 dram_h = bm.diag.dram_buffer_pool_hit.load(std::memory_order_relaxed);
      const u64 cxl_h = bm.diag.cxl_buffer_pool_hit.load(std::memory_order_relaxed);
      const u64 promotions = bm.diag.cxl_to_dram_promotions.load(std::memory_order_relaxed);
      const u64 evictions = bm.diag.evictions.load(std::memory_order_relaxed);
      const double rc_hr = (rc_h + rc_m > 0) ? (100.0 * rc_h / (rc_h + rc_m)) : 0.0;
      const double dram_hr = (lookup_count > 0) ? (100.0 * static_cast<double>(dram_h) / static_cast<double>(lookup_count)) : 0.0;
      const double cxl_hr = (lookup_count > 0) ? (100.0 * static_cast<double>(cxl_h) / static_cast<double>(lookup_count)) : 0.0;

      stats.record_cache_hit = rc_h;
      stats.record_cache_miss = rc_m;
      stats.dram_hit = dram_h;
      stats.cxl_hit = cxl_h;
      stats.cxl_to_dram_promotions = promotions;
      stats.evictions = evictions;

      if (!latency_samples_us.empty()) {
         std::sort(latency_samples_us.begin(), latency_samples_us.end());
         stats.latency_samples = latency_samples_us.size();
         const double sum_us = std::accumulate(latency_samples_us.begin(), latency_samples_us.end(), 0.0);
         stats.avg_latency_us = sum_us / static_cast<double>(latency_samples_us.size());
         stats.p95_latency_us = percentile_from_sorted(latency_samples_us, 95.0);
         stats.p99_latency_us = percentile_from_sorted(latency_samples_us, 99.0);
      }

      print_info("Measured lookups done: " + std::to_string(lookup_count) +
                 ", throughput=" + std::to_string(stats.qps / 1e6) + " Mqps");
      print_info("QPS=" + std::to_string(stats.qps) +
                 ", TPS=" + std::to_string(stats.tps));
      print_info("Latency(us) avg=" + std::to_string(stats.avg_latency_us) +
                 ", p95=" + std::to_string(stats.p95_latency_us) +
                 ", p99=" + std::to_string(stats.p99_latency_us) +
                 ", samples=" + std::to_string(stats.latency_samples));
      print_info("RecordCache hit=" + std::to_string(rc_h) +
                 ", miss=" + std::to_string(rc_m) +
                 ", hit_ratio=" + std::to_string(rc_hr) + "%");
      print_info("DRAM BufferPool hit=" + std::to_string(dram_h) +
                 ", hit_ratio_per_lookup=" + std::to_string(dram_hr) + "%");
      print_info("CXL  BufferPool hit=" + std::to_string(cxl_h) +
                 ", hit_ratio_per_lookup=" + std::to_string(cxl_hr) + "%");
      print_info("CXL->DRAM promotions=" + std::to_string(promotions) +
                 ", evictions=" + std::to_string(evictions));
      print_info("Found=" + std::to_string(founds.load()) +
                 ", NotFound=" + std::to_string(not_founds.load()) +
                 ", TX aborts=" + std::to_string(aborts.load()));
   } else {
      print_info("Warmup lookups done: " + std::to_string(lookup_count) +
                 ", throughput=" + std::to_string(lookup_count / secs / 1e6) + " Mtps");
   }
   return stats;
}

// ============================================================================
// Phase 2: Optional partition / CXL verification
// Keeping it lightweight: just verify basic tier availability.
// ============================================================================
void phase2_cxl_verification() {
   print_phase("Phase 2: CXL / Buffer Pool Verification");

   auto& bm = *storage::BMC::global_bf;
   auto* dram_bfs = bm.getDRAMBFs();
   auto* cxl_bfs = bm.getCXLBFs();

   const u64 dram_num = bm.getPoolSize();
   const u64 cxl_num = bm.getCXLPoolSize();

   u64 dram_active = 0;
   for (u64 i = 0; i < dram_num; i++) {
      if (dram_bfs[i].header.state != storage::BufferFrame::STATE::FREE) dram_active++;
   }

   print_info("DRAM frames total=" + std::to_string(dram_num) +
              ", active=" + std::to_string(dram_active));

   if (cxl_bfs && cxl_num > 0) {
      u64 cxl_active = 0;
      for (u64 i = 0; i < cxl_num; i++) {
         if (cxl_bfs[i].header.state != storage::BufferFrame::STATE::FREE) cxl_active++;
      }
      print_info("CXL frames total=" + std::to_string(cxl_num) +
                 ", active=" + std::to_string(cxl_active));
      if (cxl_active > 0) print_pass("CXL memory is being used");
      else print_warn("CXL memory has 0 active frames");
   } else {
      print_warn("CXL pool unavailable");
   }
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
   gflags::SetUsageMessage("CXL lookup test with pure record skew generator");
   gflags::ParseCommandLineFlags(&argc, &argv, true);

   if (FLAGS_test_admission_mode != "lru" &&
       FLAGS_test_admission_mode != "page_only" &&
       FLAGS_test_admission_mode != "two_level") {
      print_fail("Unsupported --test_admission_mode, expected one of: lru, page_only, two_level");
      return 1;
   }

   FLAGS_admission_mode = FLAGS_test_admission_mode;
   if (FLAGS_test_admission_mode == "lru" || FLAGS_test_admission_mode == "page_only") {
      FLAGS_enable_record_cache = false;
      FLAGS_dram_recordcache_gib = 0.0;
      FLAGS_forward_epoch_thread = 0;
      FLAGS_sieve_eviction_thread = 0;
      FLAGS_record_cache_promote_thread = 0;
      if (FLAGS_two_level_admission_threads == 0) {
         FLAGS_two_level_admission_threads = 1;
      }
   }

   if (!FLAGS_cxl_tiering_enabled) {
      print_warn("This test is intended for CXL tiering path; --cxl_tiering_enabled=false");
   }

   const Config cfg = build_config_from_flags();

   if (cfg.records_per_page() == 0) {
      print_fail("records_per_page == 0, check page_size_bytes / record_size_bytes / fill_factor");
      return 1;
   }
   if (cfg.total_records() < 2) {
      print_fail("total_records must be >= 2");
      return 1;
   }

   std::cout << "\n" << Color::BOLD << Color::CYAN
             << "============================================\n"
             << "  CXL Lookup Test (Pure Record Skew)\n"
             << "============================================"
             << Color::RESET << "\n";

   print_info("Configuration:");
   print_info("  working_set_gib       = " + std::to_string(cfg.working_set_gib));
   print_info("  record_cache_gib      = " + std::to_string(cfg.record_cache_gib));
   print_info("  zipf_theta            = " + std::to_string(cfg.zipf_theta));
   print_info("  fill_factor           = " + std::to_string(cfg.fill_factor));
   print_info("  records_per_page      = " + std::to_string(cfg.records_per_page()));
   print_info("  total_records         = " + std::to_string(cfg.total_records()));
   print_info("  total_pages           = " + std::to_string(cfg.total_pages()));
   print_info("  record_cache_capacity = " + std::to_string(cfg.record_cache_capacity()));
   print_info("  admission_mode        = " + FLAGS_admission_mode);

   print_workload_sanity_report(cfg);

   // Build generator once and reuse across phases
   ScrambledZipfGenerator generator(cfg.total_records(), cfg.zipf_theta);

   // Initialize LeanStore
   print_phase("Initialize LeanStore");
   LeanStore db;
   auto& crm = db.getCRManager();
   LeanStoreAdapter<KVTable> table;

   crm.scheduleJobSync(0, [&]() {
      table = LeanStoreAdapter<KVTable>(db, "PURE_SKEW_TEST");
   });

   print_pass(std::string("LeanStore initialized, ") + (FLAGS_vi ? "BTreeVI" : "BTreeLL"));

   // Phase 1: load data
   phase1_load_data(crm, table, cfg.total_records());

   // Optional deferred start:
   // keep admission/promote/eviction threads quiet during bulk-load,
   // then enable them right before lookup phases.
   if (FLAGS_cxl_tiering_enabled && FLAGS_delay_admission_recordcache_threads_start) {
      print_info("About to enable deferred background threads");
      auto& bm_after_load = *storage::BMC::global_bf;
      print_info("Got global_bf");
      bm_after_load.enableAdmissionAndRecordCacheThreads();
      print_info("Returned from enableAdmissionAndRecordCacheThreads()");
      print_info("Deferred background threads enabled after load phase");
   }

   // Reset diagnostics before lookup
   auto& bm = *storage::BMC::global_bf;
   bm.diag.record_cache_hit.store(0);
   bm.diag.record_cache_miss.store(0);
   bm.diag.dram_buffer_pool_hit.store(0);
   bm.diag.cxl_buffer_pool_hit.store(0);
   bm.diag.cxl_to_dram_promotions.store(0);
   bm.diag.evictions.store(0);

   // Optional CXL verification
   phase2_cxl_verification();

   // Warmup
   if (FLAGS_test_warmup_lookups > 0) {
      print_phase("Phase 3: Warmup Lookup");
      (void)run_lookup_phase(crm, table, cfg, generator, FLAGS_test_warmup_lookups, false);

      bm.diag.record_cache_hit.store(0);
      bm.diag.record_cache_miss.store(0);
      bm.diag.dram_buffer_pool_hit.store(0);
      bm.diag.cxl_buffer_pool_hit.store(0);
      bm.diag.cxl_to_dram_promotions.store(0);
      bm.diag.evictions.store(0);
   }

   // Measured
   print_phase("Phase 4: Measured Lookup");
   const auto stats = run_lookup_phase(crm, table, cfg, generator, FLAGS_test_measure_lookups, true);
   print_info("Summary: mode=" + FLAGS_admission_mode +
              ", QPS=" + std::to_string(stats.qps) +
              ", p95(us)=" + std::to_string(stats.p95_latency_us) +
              ", p99(us)=" + std::to_string(stats.p99_latency_us));

   print_pass("Test finished.");
   return 0;
}
