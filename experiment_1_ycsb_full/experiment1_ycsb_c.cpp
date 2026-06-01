// =============================================================================
// ycsb_c_test.cpp
//
// YCSB-C (100% read) workload simulation for CXL record cache experiments.
//
// Key-generation strictly follows the standard YCSB ScrambledZipfGenerator:
//   - Zipf rank sampled via the Zipfian inverse CDF (same math as YCSB Java)
//   - FNV-1a 64-bit hash for scrambling (identical to LeanStore's
//     utils/ScrambledZipfGenerator and the YCSB Java reference impl)
//   - Key space: [0, total_records)  (same as YCSB "orderedinserts=true")
//
// Data loading and background-thread startup follow the pattern established
// in test-cxl-lookup-pure-record-skew.cpp:
//   - Sequential key bulk-load [0, n), one TX per record, parallel workers
//   - Optional deferred start of admission / RecordCache threads
//     (--delay_admission_recordcache_threads_start)
//
// Ablation modes (--test_admission_mode):
//   lru        : baseline, no RecordCache, pure DRAM+CXL page tiering
//   page_only  : two-level admission without record-level cache
//   two_level  : full system (DRAM buffer pool + CXL + RecordCache)
//
// ============================================================================
// Counter accounting — IMPORTANT
// ============================================================================
//
// dram_buffer_pool_hit is incremented in BTreeVI::lookupOptimistic /
// lookupPessimistic at the point where the leaf frame is accessed:
//
//   is_in_dram = isInDRAM(leaf.bf)
//   if (is_in_dram) diag.dram_buffer_pool_hit++
//
// ssd_miss is incremented in BufferManager::resolveSwip immediately after
// readPageSync() returns.  At that point the frame has already been loaded
// into a DRAM (or CXL) BufferFrame and swizzled HOT, so by the time control
// returns to BTreeVI the frame passes isInDRAM() == true.
//
// Consequence: a single cold SSD read increments BOTH ssd_miss (resolveSwip)
// AND dram_buffer_pool_hit (BTreeVI).  This double-count is independent of
// the admission mode — it occurs in every mode because the BTree layer never
// sees the pre-load state of the frame.
//
// Correction applied uniformly in all three reporting sites:
//
//   true_dram_hit = dram_buffer_pool_hit - ssd_miss   (clamp to 0)
//
// After this adjustment the four counters are mutually exclusive and sum to
// the total number of lookup operations that reached the BTree layer:
//
//   rc_hit  +  true_dram_hit  +  cxl_hit  +  ssd_miss  =  btree_lookups
//
// In two_level mode:
//   total_lookups = rc_hit + true_dram_hit + cxl_hit + ssd_miss
//   (rc_hit covers lookups that never reached BTree at all)
//
// In page_only / lru mode:
//   rc_hit == 0 always, so total_lookups = true_dram_hit + cxl_hit + ssd_miss
//
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
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace leanstore;
using u64 = std::uint64_t;

using YCSBKey     = u64;
using YCSBPayload = BytesPayload<600>;
using YCSBTable   = Relation<YCSBKey, YCSBPayload>;

constexpr u64 GiB = 1024ULL * 1024ULL * 1024ULL;

// ============================================================================
// gflags
// ============================================================================
DEFINE_uint64(test_tuple_count,            0,              "Total records to insert (0 = auto from working set)");
DEFINE_double(test_working_set_gib,        4.0,            "Target working set size in GiB");
DEFINE_double(test_zipf_theta,             0.99,           "Zipfian skew parameter (YCSB default 0.99)");
DEFINE_uint64(test_payload_size_bytes,     100,            "Payload bytes per record (50–200); key is always 8 B");
DEFINE_double(test_fill_factor,            0.5,            "B+Tree leaf page fill factor");
DEFINE_uint64(test_warmup_lookups,         20000000ULL,    "Warmup lookup count before measured phase");
DEFINE_uint64(test_measure_lookups,        100000000ULL,   "Measured lookup count");
DEFINE_uint64(test_progress_interval,      1000000ULL,     "Print progress every N measured lookups");
DEFINE_uint64(test_warmup_progress_interval, 200000ULL,    "Print warmup progress every N warmup lookups (0 = use measured interval)");
DEFINE_uint64(test_seed,                   42ULL,          "Random seed");
DEFINE_double(test_skew_top1_threshold,    0.10,           "Threshold for top-1% page identification");
DEFINE_uint64(test_sample_count,           10000000ULL,    "Sample count for workload sanity report");
DEFINE_string(test_admission_log,          "admission_control_log.csv", "Output file for admission stats");
DEFINE_string(test_hit_stats_log,          "hit_stats.csv",             "Output file for hit-rate stats");
DEFINE_string(test_admission_mode,         "two_level",    "Ablation mode: lru | page_only | two_level");

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
}

void print_info(const std::string& msg)  { std::cout << Color::CYAN   << "[INFO] " << Color::RESET << msg << "\n"; }
void print_warn(const std::string& msg)  { std::cout << Color::YELLOW << "[WARN] " << Color::RESET << msg << "\n"; }
void print_pass(const std::string& msg)  { std::cout << Color::GREEN  << "[PASS] " << Color::RESET << msg << "\n"; }
void print_fail(const std::string& msg)  { std::cout << Color::RED    << "[FAIL] " << Color::RESET << msg << "\n"; }
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
   double      fill_factor             = 0.5;
   double      working_set_gib         = 4.0;
   double      record_cache_gib        = 1.0;
   double      zipf_theta              = 0.99;
   u64         payload_size_bytes      = 100;
   u64         sample_count            = 10000000ULL;
   u64         seed                    = 42ULL;
   double      skew_top1_threshold     = 0.10;
   std::string admission_mode          = "two_level";

   static constexpr u64 BTREE_PAGE_SIZE             = 16384;
   static constexpr u64 BTREE_EFFECTIVE_PAGE_SIZE   = 16352;
   static constexpr u64 BTREE_NODE_HEADER_BYTES     = 96;
   static constexpr u64 BTREE_SLOT_BYTES            = 10;
   static constexpr u64 BTREE_CHAINED_TUPLE_BYTES   = 25;
   static constexpr u64 RECORD_CACHE_ENTRY_BYTES    = 16;
   static constexpr u64 BTREE_USABLE_BYTES_PER_PAGE =
       BTREE_EFFECTIVE_PAGE_SIZE - BTREE_NODE_HEADER_BYTES;

   u64 per_record_on_page_bytes() const {
      return BTREE_SLOT_BYTES + sizeof(YCSBKey) + BTREE_CHAINED_TUPLE_BYTES + payload_size_bytes;
   }
   u64 records_per_page() const {
      return static_cast<u64>(
          static_cast<double>(BTREE_USABLE_BYTES_PER_PAGE) * fill_factor /
          static_cast<double>(per_record_on_page_bytes()));
   }
   u64 total_pages() const {
      return static_cast<u64>(working_set_gib * GiB) / BTREE_PAGE_SIZE;
   }
   u64 total_records() const {
      return total_pages() * records_per_page();
   }
   u64 rc_entry_bytes() const {
      return RECORD_CACHE_ENTRY_BYTES + sizeof(YCSBKey) + payload_size_bytes;
   }
   u64 record_cache_capacity() const {
      return static_cast<u64>(record_cache_gib * GiB) / rc_entry_bytes();
   }
};

Config build_config_from_flags() {
   if (FLAGS_test_payload_size_bytes < 50 || FLAGS_test_payload_size_bytes > 200) {
      print_fail("--test_payload_size_bytes must be in [50, 200]");
      std::exit(1);
   }
   Config cfg;
   cfg.fill_factor         = FLAGS_test_fill_factor;
   cfg.working_set_gib     = FLAGS_test_working_set_gib;
   cfg.record_cache_gib    = FLAGS_dram_recordcache_gib;
   cfg.zipf_theta          = FLAGS_test_zipf_theta;
   cfg.payload_size_bytes  = FLAGS_test_payload_size_bytes;
   cfg.sample_count        = FLAGS_test_sample_count;
   cfg.seed                = FLAGS_test_seed;
   cfg.skew_top1_threshold = FLAGS_test_skew_top1_threshold;
   cfg.admission_mode      = FLAGS_test_admission_mode;
   return cfg;
}

// ============================================================================
// Counter correction helper
// ============================================================================
//
// Raw counters from diag:
//   dram_buffer_pool_hit : incremented in BTreeVI when isInDRAM(leaf.bf)==true
//   ssd_miss             : incremented in resolveSwip after readPageSync()
//
// Problem: a cold SSD read loads the page into a DRAM frame, then BTreeVI
// sees isInDRAM()==true and increments dram_buffer_pool_hit.  So both
// ssd_miss and dram_buffer_pool_hit are incremented for the same lookup.
//
// Fix: subtract ssd_miss from dram_buffer_pool_hit to get the count of
// lookups that found the page *already warm* in DRAM (no SSD I/O needed).
//
// This correction is mode-independent: the double-count happens in the
// BTree/BufferManager layer which is traversed in ALL modes (including
// two_level, because RC-miss lookups also go through BTree).
//
struct CorrectedCounters {
   u64 rc_hit        = 0;   // RecordCache hits (two_level only; 0 in other modes)
   u64 rc_miss       = 0;   // RecordCache misses
   u64 true_dram_hit = 0;   // Pages already warm in DRAM (cold SSD reads excluded)
   u64 cxl_hit       = 0;   // Pages served from CXL
   u64 ssd_miss      = 0;   // Cold SSD reads (page not in any memory tier)
   u64 promotions    = 0;   // CXL→DRAM promotions
   u64 record_promote_requests = 0; // slot-level promote requests dispatched
   u64 decision_full_page      = 0; // two-level full-page decisions
   u64 decision_record_page    = 0; // two-level record-page decisions
   u64 decision_record_slots   = 0; // two-level record-slot decisions
   u64 add_candidate_lock_fail = 0;
   u64 on_record_lock_fail     = 0;
   u64 check_promote_lock_fail = 0;
   u64 rc_promote_enqueued     = 0;
   u64 rc_promote_dequeued     = 0;
   u64 rc_promote_success      = 0;
   u64 rc_promote_drop_existing_conflict = 0;
   u64 rc_promote_drop_alloc_fail        = 0;
   u64 rc_promote_drop_postcheck_state   = 0;
   u64 rc_promote_skip_inflight          = 0;
   u64 rc_promote_skip_cooldown          = 0;
   u64 rc_miss_not_present               = 0;
   u64 rc_miss_hash_mismatch             = 0;
   u64 rc_miss_stale_or_invisible        = 0;
   u64 rc_miss_evicted_recently          = 0;
   u64 rc_miss_bypassed_policy           = 0;
   u64 evictions     = 0;   // Evictions from DRAM
   u64 sieve_eviction_entries = 0; // RecordCache entries reclaimed by SIEVE thread
   u64 total         = 0;   // rc_hit + true_dram_hit + cxl_hit + ssd_miss

   double rc_hr      = 0.0; // %
   double dram_hr    = 0.0; // %
   double cxl_hr     = 0.0; // %
   double ssd_rate   = 0.0; // %
};

struct PromoteEfficiencyMetrics {
   double useful_promote = 0.0; // success_new_insert / total_promote_req
   double wasted_promote = 0.0; // (drop_existing + skip_inflight + late_reject) / total_promote_req
};

static PromoteEfficiencyMetrics compute_promote_efficiency_metrics(const CorrectedCounters& c)
{
   PromoteEfficiencyMetrics m;
   if (c.record_promote_requests == 0) {
      return m;
   }
   const double total_req = static_cast<double>(c.record_promote_requests);
   const double success_new_insert = static_cast<double>(c.rc_promote_success);
   const double late_reject = static_cast<double>(c.rc_promote_drop_postcheck_state);
   const double wasted = static_cast<double>(c.rc_promote_drop_existing_conflict) +
                         static_cast<double>(c.rc_promote_skip_inflight) +
                         late_reject;
   m.useful_promote = success_new_insert / total_req;
   m.wasted_promote = wasted / total_req;
   return m;
}

// Read raw diag counters and apply the double-count correction uniformly.
// This is the single authoritative place for the correction — all three
// reporting sites (progress print, run_lookup_phase summary, main summary)
// call this function instead of doing their own ad-hoc arithmetic.
static CorrectedCounters read_and_correct_counters()
{
   auto& bm = *storage::BMC::global_bf;

   const u64 rc_h       = bm.diag.record_cache_hit.load(std::memory_order_relaxed);
   const u64 rc_m       = bm.diag.record_cache_miss.load(std::memory_order_relaxed);
   const u64 raw_dram_h = bm.diag.dram_buffer_pool_hit.load(std::memory_order_relaxed);
   const u64 cxl_h      = bm.diag.cxl_buffer_pool_hit.load(std::memory_order_relaxed);
   const u64 ssd_m      = bm.diag.ssd_miss.load(std::memory_order_relaxed);
   const u64 promotions = bm.diag.cxl_to_dram_promotions.load(std::memory_order_relaxed);
   const u64 evictions  = bm.diag.evictions.load(std::memory_order_relaxed);
   const u64 sieve_eviction_entries =
       (bm.global_record_cache != nullptr) ? bm.global_record_cache->getSieveEvictionEntries() : 0;
   u64 add_candidate_lock_fail = 0;
   u64 on_record_lock_fail = 0;
   u64 check_promote_lock_fail = 0;
   u64 rc_promote_enqueued = 0;
   u64 rc_promote_dequeued = 0;
   u64 rc_promote_success = 0;
   u64 rc_promote_drop_existing_conflict = 0;
   u64 rc_promote_drop_alloc_fail = 0;
   u64 rc_promote_drop_postcheck_state = 0;
   u64 rc_promote_skip_inflight = 0;
   u64 rc_promote_skip_cooldown = 0;
   const u64 record_promote_requests = 0;
   const u64 decision_full_page = 0;
   const u64 decision_record_page = 0;
   const u64 decision_record_slots = 0;
   const u64 rc_miss_not_present = 0;
   const u64 rc_miss_hash_mismatch = 0;
   const u64 rc_miss_stale_or_invisible = 0;
   const u64 rc_miss_evicted_recently = 0;
   const u64 rc_miss_bypassed_policy = 0;

   // Correction: dram_buffer_pool_hit includes SSD-cold-read pages that were
   // loaded into DRAM by resolveSwip before BTreeVI checked isInDRAM().
   // Subtract ssd_miss to recover the true warm-DRAM hit count.
   // Clamp to 0 to guard against any transient counter ordering artefacts
   // (the atomic increments in resolveSwip and BTreeVI are not fenced against
   // each other, so a reader snapshot could momentarily see ssd_m > raw_dram_h
   // by one or two counts under heavy concurrency).
   const u64 true_dram_h = (raw_dram_h >= ssd_m) ? (raw_dram_h - ssd_m) : 0;
   const u64 total       = rc_h + true_dram_h + cxl_h + ssd_m;

   CorrectedCounters c;
   c.rc_hit        = rc_h;
   c.rc_miss       = rc_m;
   c.true_dram_hit = true_dram_h;
   c.cxl_hit       = cxl_h;
   c.ssd_miss      = ssd_m;
   c.promotions    = promotions;
   c.record_promote_requests = record_promote_requests;
   c.decision_full_page = decision_full_page;
   c.decision_record_page = decision_record_page;
   c.decision_record_slots = decision_record_slots;
   c.add_candidate_lock_fail = add_candidate_lock_fail;
   c.on_record_lock_fail = on_record_lock_fail;
   c.check_promote_lock_fail = check_promote_lock_fail;
   c.rc_promote_enqueued = rc_promote_enqueued;
   c.rc_promote_dequeued = rc_promote_dequeued;
   c.rc_promote_success = rc_promote_success;
   c.rc_promote_drop_existing_conflict = rc_promote_drop_existing_conflict;
   c.rc_promote_drop_alloc_fail = rc_promote_drop_alloc_fail;
   c.rc_promote_drop_postcheck_state = rc_promote_drop_postcheck_state;
   c.rc_promote_skip_inflight = rc_promote_skip_inflight;
   c.rc_promote_skip_cooldown = rc_promote_skip_cooldown;
   c.rc_miss_not_present = rc_miss_not_present;
   c.rc_miss_hash_mismatch = rc_miss_hash_mismatch;
   c.rc_miss_stale_or_invisible = rc_miss_stale_or_invisible;
   c.rc_miss_evicted_recently = rc_miss_evicted_recently;
   c.rc_miss_bypassed_policy = rc_miss_bypassed_policy;
   c.evictions     = evictions;
    c.sieve_eviction_entries = sieve_eviction_entries;
   c.total         = total;

   if (total > 0) {
      c.rc_hr    = 100.0 * static_cast<double>(rc_h)        / static_cast<double>(total);
      c.dram_hr  = 100.0 * static_cast<double>(true_dram_h) / static_cast<double>(total);
      c.cxl_hr   = 100.0 * static_cast<double>(cxl_h)       / static_cast<double>(total);
      c.ssd_rate = 100.0 * static_cast<double>(ssd_m)        / static_cast<double>(total);
   }
   return c;
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
      return static_cast<double>(next()) /
             static_cast<double>(std::numeric_limits<u64>::max());
   }
private:
   u64 state_;
};

// ============================================================================
// FNV-1a 64-bit hash (YCSB Java / LeanStore ScrambledZipfGenerator)
// ============================================================================
static u64 fnvHash64(u64 val) {
   constexpr u64 FNV_OFFSET_BASIS = 0xCBF29CE484222325ULL;
   constexpr u64 FNV_PRIME        = 1099511628211ULL;
   u64 h = FNV_OFFSET_BASIS;
   for (int i = 0; i < 8; i++) {
      h ^= (val & 0xffULL);
      h *= FNV_PRIME;
      val >>= 8;
   }
   return h;
}

// ============================================================================
// ZipfGenerator — inverse CDF method (same as YCSB Java ZipfianGenerator)
// ============================================================================
class ZipfGenerator {
public:
   ZipfGenerator() = default;
   ZipfGenerator(u64 n, double theta) { reset(n, theta); }

   void reset(u64 n, double theta) {
      if (n < 2) throw std::invalid_argument("ZipfGenerator: n must be >= 2");
      n_      = n;
      theta_  = theta;
      alpha_  = 1.0 / (1.0 - theta);
      zetan_  = zeta(n, theta);
      eta_    = (1.0 - std::pow(2.0 / static_cast<double>(n), 1.0 - theta)) /
                (1.0 - zeta(2, theta) / zetan_);
   }

   u64 next(SplitMix64& rng) const {
      const double u  = rng.next01();
      const double uz = u * zetan_;
      if (uz < 1.0) return 1;
      if (uz < 1.0 + std::pow(0.5, theta_)) return 2;
      return 1 + static_cast<u64>(
          static_cast<double>(n_) * std::pow(eta_ * u - eta_ + 1.0, alpha_));
   }

private:
   static double zeta(u64 n, double theta) {
      double sum = 0.0;
      for (u64 i = 1; i <= n; i++)
         sum += std::pow(1.0 / static_cast<double>(i), theta);
      return sum;
   }

   u64    n_      = 0;
   double theta_  = 0.0;
   double alpha_  = 0.0;
   double zetan_  = 0.0;
   double eta_    = 0.0;
};

// ============================================================================
// ScrambledZipfGenerator — YCSB-standard key generator
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
   u64           n_ = 0;
   ZipfGenerator zipf_;
};

// ============================================================================
// Theoretical top-K hit ratio under Zipf(theta)
// ============================================================================
static double theoretical_topk_hit_ratio(u64 total_records, u64 rc_capacity, double theta) {
   const u64 K           = std::min(rc_capacity, total_records);
   const u64 exact_limit = std::min(total_records, u64{20000000});

   double zeta_n = 0.0;
   double zeta_k = 0.0;

   for (u64 i = 1; i <= exact_limit; i++) {
      const double term = std::pow(1.0 / static_cast<double>(i), theta);
      zeta_n += term;
      if (i <= K) zeta_k += term;
   }

   if (total_records > exact_limit) {
      const double L    = static_cast<double>(exact_limit);
      const double N    = static_cast<double>(total_records);
      const double exp  = 1.0 - theta;
      const double tail = (std::pow(N + 0.5, exp) - std::pow(L + 0.5, exp)) / exp;
      zeta_n += tail;

      if (K > exact_limit) {
         const double Kd     = static_cast<double>(K);
         const double tail_k = (std::pow(Kd + 0.5, exp) - std::pow(L + 0.5, exp)) / exp;
         zeta_k += tail_k;
      }
   }

   return (zeta_n > 0.0) ? (zeta_k / zeta_n) : 0.0;
}

struct TheoreticalTargets {
   double rc_hr_upper_pct = 0.0;
   double dram_page_hr_upper_pct = 0.0;
};

static TheoreticalTargets build_theoretical_targets(const Config& cfg) {
   TheoreticalTargets t;
   const u64 total_records = cfg.total_records();
   const u64 total_pages = cfg.total_pages();
   const u64 rc_capacity = cfg.record_cache_capacity();
   const u64 dram_total_pages = static_cast<u64>(
       (FLAGS_dram_buffer_pool_gib + FLAGS_dram_recordcache_gib) *
       static_cast<double>(GiB) /
       static_cast<double>(Config::BTREE_PAGE_SIZE));
   t.rc_hr_upper_pct = theoretical_topk_hit_ratio(total_records, rc_capacity, cfg.zipf_theta) * 100.0;
   t.dram_page_hr_upper_pct = theoretical_topk_hit_ratio(total_pages, dram_total_pages, cfg.zipf_theta) * 100.0;
   return t;
}

// ============================================================================
// Workload sanity report
// ============================================================================
void print_workload_sanity_report(const Config& cfg) {
   const u64    total_records = cfg.total_records();
   const u64    total_pages   = cfg.total_pages();
   const u64    rc_capacity   = cfg.record_cache_capacity();

   const double rc_topk_hr = theoretical_topk_hit_ratio(
       total_records, rc_capacity, cfg.zipf_theta) * 100.0;

   const u64 dram_total_pages = static_cast<u64>(
       (FLAGS_dram_buffer_pool_gib + FLAGS_dram_recordcache_gib) *
       static_cast<double>(GiB) /
       static_cast<double>(Config::BTREE_PAGE_SIZE));
   const double dram_page_hr = theoretical_topk_hit_ratio(
       total_pages, dram_total_pages, cfg.zipf_theta) * 100.0;

   print_phase("Workload Sanity Report");
   print_info("working_set_gib           = " + std::to_string(cfg.working_set_gib) +
              " GiB  (= " + std::to_string(total_pages) + " pages x " +
              std::to_string(Config::BTREE_PAGE_SIZE) + " B/page)");
   print_info("total_pages (N_page)      = " + std::to_string(total_pages));
   print_info("per_record_on_page_bytes  = " + std::to_string(cfg.per_record_on_page_bytes()) +
              " B  (Slot=" + std::to_string(Config::BTREE_SLOT_BYTES) +
              " + key_suffix=8 + ChainedTuple=" + std::to_string(Config::BTREE_CHAINED_TUPLE_BYTES) +
              " + payload=" + std::to_string(cfg.payload_size_bytes) + ")");
   print_info("records_per_page          = " + std::to_string(cfg.records_per_page()) +
              "  (usable=" + std::to_string(Config::BTREE_USABLE_BYTES_PER_PAGE) +
              " B x fill=" + std::to_string(cfg.fill_factor) + ")");
   print_info("total_records (N_rec)     = " + std::to_string(total_records));
   print_info("record_cache_gib          = " + std::to_string(cfg.record_cache_gib));
   print_info("rc_entry_bytes            = " + std::to_string(cfg.rc_entry_bytes()) +
              " B  (RecordCacheEntry=" + std::to_string(Config::RECORD_CACHE_ENTRY_BYTES) +
              " + key=8 + payload=" + std::to_string(cfg.payload_size_bytes) + ")");
   print_info("record_cache_capacity (K_rec) = " + std::to_string(rc_capacity));
   print_info("zipf_theta                = " + std::to_string(cfg.zipf_theta));

   if (rc_capacity >= total_records) {
      print_warn("RC capacity (K_rec=" + std::to_string(rc_capacity) +
                 ") >= total_records (N_rec=" + std::to_string(total_records) +
                 "): theoretical RC HR = 100% (RC larger than working set)");
      print_warn("Fix: increase working_set_gib OR decrease record_cache_gib.");
   } else {
      print_info("theoretical_RC_HR_upper   = " + std::to_string(rc_topk_hr) + "%" +
                 "  [top-" + std::to_string(rc_capacity) + " of " +
                 std::to_string(total_records) + " records]");
   }

   print_info("dram_buffer_pool_gib      = " + std::to_string(FLAGS_dram_buffer_pool_gib));
   print_info("dram_recordcache_gib      = " + std::to_string(FLAGS_dram_recordcache_gib));
   print_info("dram_total_pages (K_page) = " + std::to_string(dram_total_pages));
   if (dram_total_pages >= total_pages) {
      print_warn("DRAM capacity (K_page=" + std::to_string(dram_total_pages) +
                 ") >= total_pages (N_page=" + std::to_string(total_pages) +
                 "): theoretical DRAM page HR = 100%");
   } else {
      print_info("theoretical_DRAM_page_HR_upper = " + std::to_string(dram_page_hr) + "%" +
                 "  [top-" + std::to_string(dram_total_pages) + " of " +
                 std::to_string(total_pages) + " pages]");
   }

   const double dram_tier_total_gib = FLAGS_dram_buffer_pool_gib + FLAGS_dram_recordcache_gib;
   const double cxl_tier_gib        = FLAGS_cxl_tiering_enabled ? FLAGS_cxl_gib : 0.0;

   print_info("--- Storage Tier Capacity ---");
   print_info("DRAM tier total           = " + std::to_string(dram_tier_total_gib) +
              " GiB  (bp=" + std::to_string(FLAGS_dram_buffer_pool_gib) +
              " GiB + rc=" + std::to_string(FLAGS_dram_recordcache_gib) + " GiB)");
   if (FLAGS_cxl_tiering_enabled) {
      print_info("CXL  tier total           = " + std::to_string(cxl_tier_gib) + " GiB");
   } else {
      print_info("CXL  tier total           = N/A  (cxl_tiering_enabled=false)");
   }
   print_info("SSD  working set          = " + std::to_string(cfg.working_set_gib) +
              " GiB  (= " + std::to_string(total_pages) + " pages x " +
              std::to_string(Config::BTREE_PAGE_SIZE) + " B/page)");
   print_info("seed                      = " + std::to_string(cfg.seed));
   print_info("generator                 = ScrambledZipf (FNV-1a, YCSB-standard)");
}

// ============================================================================
// Stats
// ============================================================================
struct LookupRunStats {
   double secs              = 0.0;
   u64    lookups           = 0;
   u64    aborts            = 0;
   u64    found             = 0;
   u64    not_found         = 0;
   double qps               = 0.0;
   double tps               = 0.0;
   double avg_latency_us    = 0.0;
   double p95_latency_us    = 0.0;
   double p99_latency_us    = 0.0;
   u64    latency_samples   = 0;

   // Corrected counters — stored directly so main() does not need to re-read diag.
   CorrectedCounters counters;
};

// ============================================================================
// LatencyHistogram
// ============================================================================
class LatencyHistogram {
public:
   static constexpr u64 BOUNDS[] = {
         1,       2,       3,       5,       7,      10,      15,      22,
        33,      50,      75,     112,     168,     252,     378,     567,
       850,    1275,    1912,    2868,    4302,    6453,    9679,   14519,
     21778,   32667,   49000,   73500,  110250,  165375,  248062,  372093,
    558139,  837208, 1255812, 1883718, 2000000
   };
   static constexpr int NUM_BOUNDS  = static_cast<int>(sizeof(BOUNDS) / sizeof(BOUNDS[0]));
   static constexpr int NUM_BUCKETS = NUM_BOUNDS + 1;

   LatencyHistogram() {
      for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
      sum_us_.store(0, std::memory_order_relaxed);
      count_.store(0, std::memory_order_relaxed);
   }

   void record(u64 latency_us) {
      buckets_[latency_us_to_bucket(latency_us)].fetch_add(1, std::memory_order_relaxed);
      sum_us_.fetch_add(latency_us, std::memory_order_relaxed);
      count_.fetch_add(1, std::memory_order_relaxed);
   }

   void merge(const LatencyHistogram& other) {
      for (int i = 0; i < NUM_BUCKETS; i++)
         buckets_[i].fetch_add(
             other.buckets_[i].load(std::memory_order_relaxed),
             std::memory_order_relaxed);
      sum_us_.fetch_add(other.sum_us_.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
      count_.fetch_add(other.count_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
   }

   double avg_us() const {
      const u64 n = count_.load(std::memory_order_relaxed);
      if (n == 0) return 0.0;
      return static_cast<double>(sum_us_.load(std::memory_order_relaxed)) /
             static_cast<double>(n);
   }

   double percentile_us(double pct) const {
      const u64 total = count_.load(std::memory_order_relaxed);
      if (total == 0) return 0.0;
      const u64 target = static_cast<u64>(
          std::ceil(pct / 100.0 * static_cast<double>(total)));
      u64 cumulative = 0;
      for (int i = 0; i < NUM_BUCKETS; i++) {
         const u64 bucket_count = buckets_[i].load(std::memory_order_relaxed);
         cumulative += bucket_count;
         if (cumulative >= target) {
            const double lower = (i == 0) ? 0.0 : static_cast<double>(BOUNDS[i - 1]);
            const double upper = (i < NUM_BOUNDS)
                                     ? static_cast<double>(BOUNDS[i])
                                     : static_cast<double>(BOUNDS[NUM_BOUNDS - 1]);
            const u64    prev  = cumulative - bucket_count;
            const double frac  = (bucket_count > 0)
                ? static_cast<double>(target - prev) /
                  static_cast<double>(bucket_count)
                : 0.5;
            return lower + frac * (upper - lower);
         }
      }
      return static_cast<double>(BOUNDS[NUM_BOUNDS - 1]);
   }

   u64 total_count() const { return count_.load(std::memory_order_relaxed); }

private:
   static int latency_us_to_bucket(u64 latency_us) {
      const u64* pos = std::lower_bound(BOUNDS, BOUNDS + NUM_BOUNDS, latency_us);
      return static_cast<int>(pos - BOUNDS);
   }

   std::atomic<u64> buckets_[NUM_BUCKETS];
   std::atomic<u64> sum_us_;
   std::atomic<u64> count_;
};

// ============================================================================
// Phase 1: Bulk load
// ============================================================================
void phase1_load_data(cr::CRManager& crm,
                      LeanStoreAdapter<YCSBTable>& table,
                      u64 tuple_count,
                      u64 payload_size_bytes)
{
   print_phase("Phase 1: Load Data");
   auto t0 = std::chrono::high_resolution_clock::now();

   utils::Parallelize::range(
       FLAGS_worker_threads, tuple_count,
       [&](u64 t_i, u64 range_begin, u64 range_end) {
          crm.scheduleJobAsync(t_i, [&, range_begin, range_end]() {
             for (u64 i = range_begin; i < range_end; i++) {
                YCSBPayload payload;
                std::memset(payload.value, 0, sizeof(payload.value));
                utils::RandomGenerator::getRandString(payload.value, payload_size_bytes);
                cr::Worker::my().startTX(TX_MODE::OLTP,
                                         TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
                table.insert_var({i}, {payload}, payload_size_bytes);
                cr::Worker::my().commitTX();
             }
          });
       });

   crm.joinAll();

   const double secs = std::chrono::duration<double>(
       std::chrono::high_resolution_clock::now() - t0).count();
   print_info("Loaded " + std::to_string(tuple_count) + " records in " +
              std::to_string(secs) + " s  (" +
              std::to_string(tuple_count / secs / 1e6) + " M rec/s)");
}

// ============================================================================
// Reset diagnostic counters between phases
// ============================================================================
static void reset_diag_counters()
{
   auto& bm = *storage::BMC::global_bf;
   bm.diag.record_cache_hit.store(0,            std::memory_order_relaxed);
   bm.diag.record_cache_miss.store(0,           std::memory_order_relaxed);
   bm.diag.dram_buffer_pool_hit.store(0,        std::memory_order_relaxed);
   bm.diag.cxl_buffer_pool_hit.store(0,         std::memory_order_relaxed);
   bm.diag.ssd_miss.store(0,                    std::memory_order_relaxed);
   bm.diag.cxl_to_dram_promotions.store(0,      std::memory_order_relaxed);
   bm.diag.evictions.store(0,                   std::memory_order_relaxed);
}

// ============================================================================
// Lookup phase — YCSB-C: 100% point reads with ScrambledZipf key distribution
// ============================================================================
LookupRunStats run_lookup_phase(cr::CRManager& crm,
                                LeanStoreAdapter<YCSBTable>& table,
                                const Config& cfg,
                                const TheoreticalTargets& theoretical_targets,
                                const ScrambledZipfGenerator& generator,
                                u64 lookup_count,
                                bool measured)
{
   std::atomic<u64> done{0};
   std::atomic<u64> aborts{0};
   std::atomic<u64> founds{0};
   std::atomic<u64> not_founds{0};
   LatencyHistogram  global_histogram;

   auto t0 = std::chrono::high_resolution_clock::now();

   const u64 thread_count = static_cast<u64>(FLAGS_worker_threads);
   const u64 base_quota   = lookup_count / thread_count;
   const u64 remainder    = lookup_count % thread_count;

   for (u64 t_i = 0; t_i < thread_count; t_i++) {
      const u64 quota = base_quota + (t_i < remainder ? 1 : 0);

      crm.scheduleJobAsync(t_i, [&, t_i, quota]() {
         const u64 phase_seed = cfg.seed ^
                                (measured ? 0xD1B54A32D192ED03ULL
                                          : 0x9E3779B97F4A7C15ULL);
         SplitMix64       rng(phase_seed + 0x9e3779b97f4a7c15ULL * (t_i + 1));
         LatencyHistogram local_histogram;
         // volatile: must survive longjmp — modified inside jumpmuTry, read
         // in the while-condition after jumpmuCatch.  Without volatile the
         // compiler may keep it in a register that longjmp restores to a
         // stale value, causing undefined behaviour (GCC -Wclobbered).
         volatile u64     local_done = 0;

         while (local_done < quota) {
            // Snapshot RNG state *before* setjmp so that a longjmp (TX
            // abort) replays the same key instead of advancing the RNG
            // into an inconsistent state.
            const auto rng_snapshot = rng;

            jumpmuTry() {
               const auto t_begin =
                   measured ? std::chrono::high_resolution_clock::now()
                            : std::chrono::high_resolution_clock::time_point{};

               const YCSBKey key   = generator.next(rng);
               bool          found = false;

               cr::Worker::my().startTX(TX_MODE::OLTP,
                                         TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
               table.lookup1({key}, [&](const YCSBTable&) { found = true; });
               cr::Worker::my().commitTX();

               if (found) founds.fetch_add(1, std::memory_order_relaxed);
               else       not_founds.fetch_add(1, std::memory_order_relaxed);

               local_done++;

               if (measured) {
                  const u64 elapsed_us = static_cast<u64>(
                      std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::high_resolution_clock::now() - t_begin).count());
                  local_histogram.record(elapsed_us);
               }

               // ------------------------------------------------------------------
               // Progress reporting
               // ------------------------------------------------------------------
               const u64 global_done =
                   done.fetch_add(1, std::memory_order_relaxed) + 1;

               const u64 progress_interval = measured
                                                 ? FLAGS_test_progress_interval
                                                 : (FLAGS_test_warmup_progress_interval > 0
                                                        ? FLAGS_test_warmup_progress_interval
                                                        : FLAGS_test_progress_interval);
               if (progress_interval > 0 &&
                   global_done % progress_interval == 0) {

                  const double elapsed_secs = std::chrono::duration<double>(
                      std::chrono::high_resolution_clock::now() - t0).count();
                  const double current_qps =
                      (elapsed_secs > 0.0)
                      ? (static_cast<double>(global_done) / elapsed_secs)
                      : 0.0;

                  const bool is_two_level = (FLAGS_test_admission_mode == "two_level");
                  u64 fine_grained_threshold = 0;
                  u64 dram_hot_candidates    = 0;
                  u64 l2_skew_promote_count = 0;
                  u64 l2_uniform_promote_count = 0;
                  u64 l2_timeout_removed_count = 0;
                  if (auto* wrapper = storage::BMC::global_bf->getAdmissionControlWrapper();
                      is_two_level && wrapper != nullptr) {
                     fine_grained_threshold =
                         wrapper->GetHistogram().GetAdmissionThreshold_fine();
                     auto& hot_candidates = wrapper->GetHotPageCandidates();
                     dram_hot_candidates = hot_candidates.GetCandidatesSize();
                     l2_skew_promote_count = hot_candidates.GetSkewPromoteCount();
                     l2_uniform_promote_count = hot_candidates.GetUniformPromoteCount();
                     l2_timeout_removed_count = hot_candidates.GetTimeoutRemovedCount();
                  }

                  if (!measured) {
                     // Warmup: simple progress line
                     std::string warmup_msg =
                         "[Warmup] " + std::to_string(global_done) + "/" +
                         std::to_string(lookup_count) +
                         " (" +
                         std::to_string(100.0 * static_cast<double>(global_done) /
                                        static_cast<double>(lookup_count)) +
                         "%)"
                         ", elapsed=" + std::to_string(elapsed_secs) + "s" +
                         ", throughput=" + std::to_string(current_qps / 1e6) + " Mqps";
                     if (is_two_level) {
                        warmup_msg +=
                            ", [L1] threshold=" + std::to_string(fine_grained_threshold) +
                            " candidates_total=" + std::to_string(dram_hot_candidates) +
                            ", [L2] skew_promote=" + std::to_string(l2_skew_promote_count) +
                            " uniform_promote=" + std::to_string(l2_uniform_promote_count) +
                            " timeout_removed=" + std::to_string(l2_timeout_removed_count);
                     }
                     print_info(warmup_msg);
                  } else {
                     // Measured: full diagnostic output using corrected counters.
                     // read_and_correct_counters() is the single authoritative source;
                     // no ad-hoc arithmetic here.
                     const CorrectedCounters c = read_and_correct_counters();
                     const double rc_gap_to_theory =
                         std::max(0.0, theoretical_targets.rc_hr_upper_pct - c.rc_hr);
                     const double dram_gap_to_theory =
                         std::max(0.0, theoretical_targets.dram_page_hr_upper_pct - c.dram_hr);

                     if (is_two_level) {
                        print_info(
                            "Progress " + std::to_string(global_done) + "/" +
                            std::to_string(lookup_count) +
                            ", throughput=" + std::to_string(current_qps / 1e6) + " Mqps" +
                            ", RC_HR="   + std::to_string(c.rc_hr)   + "%" +
                            ", RC_gap_to_theory=" + std::to_string(rc_gap_to_theory) + "pp" +
                            ", DRAM_HR=" + std::to_string(c.dram_hr) + "%" +
                            ", DRAM_gap_to_theory=" + std::to_string(dram_gap_to_theory) + "pp" +
                            ", CXL_HR="  + std::to_string(c.cxl_hr)  + "%" +
                            ", SSD_miss="+ std::to_string(c.ssd_rate)+ "%" +
                            " [rc="      + std::to_string(c.rc_hit)  +
                            " dram="     + std::to_string(c.true_dram_hit) +
                            " cxl="      + std::to_string(c.cxl_hit) +
                            " ssd="      + std::to_string(c.ssd_miss)+
                            " total="    + std::to_string(c.total)   + "]" +
                            ", fine_threshold="    + std::to_string(fine_grained_threshold) +
                            ", dram_hot_candidates=" + std::to_string(dram_hot_candidates) +
                            ", [L1] threshold=" + std::to_string(fine_grained_threshold) +
                            " candidates_total=" + std::to_string(dram_hot_candidates) +
                            ", [L2] skew_promote=" + std::to_string(l2_skew_promote_count) +
                            " uniform_promote=" + std::to_string(l2_uniform_promote_count) +
                            " timeout_removed=" + std::to_string(l2_timeout_removed_count));
                     } else {
                        print_info(
                            "Progress " + std::to_string(global_done) + "/" +
                            std::to_string(lookup_count) +
                            ", throughput=" + std::to_string(current_qps / 1e6) + " Mqps" +
                            ", DRAM_HR=" + std::to_string(c.dram_hr) + "%" +
                            ", DRAM_gap_to_theory=" + std::to_string(dram_gap_to_theory) + "pp" +
                            ", CXL_HR="  + std::to_string(c.cxl_hr)  + "%" +
                            ", SSD_miss="+ std::to_string(c.ssd_rate)+ "%" +
                            " [dram="    + std::to_string(c.true_dram_hit) +
                            " cxl="      + std::to_string(c.cxl_hit) +
                            " ssd="      + std::to_string(c.ssd_miss)+
                            " total="    + std::to_string(c.total)   + "]"
                            );
                     }
                  }
               }
            }
            jumpmuCatch() {
               // Restore RNG to pre-setjmp snapshot so the next iteration
               // retries with a consistent generator state.
               rng = rng_snapshot;
               aborts.fetch_add(1, std::memory_order_relaxed);
            }
         } // while local_done < quota

         if (measured) {
            global_histogram.merge(local_histogram);
         }
      }); // scheduleJobAsync
   }

   crm.joinAll();

   const double secs = std::chrono::duration<double>(
       std::chrono::high_resolution_clock::now() - t0).count();

   LookupRunStats stats;
   stats.secs      = secs;
   stats.lookups   = lookup_count;
   stats.aborts    = aborts.load(std::memory_order_relaxed);
   stats.found     = founds.load(std::memory_order_relaxed);
   stats.not_found = not_founds.load(std::memory_order_relaxed);
   stats.qps       = (secs > 0.0) ? (static_cast<double>(lookup_count) / secs) : 0.0;
   stats.tps       = stats.qps;

   if (measured) {
      // Read and correct counters once — stored in stats for main() to use.
      stats.counters = read_and_correct_counters();

      // Latency percentiles from the merged histogram.
      stats.latency_samples = global_histogram.total_count();
      stats.avg_latency_us  = global_histogram.avg_us();
      stats.p95_latency_us  = global_histogram.percentile_us(95.0);
      stats.p99_latency_us  = global_histogram.percentile_us(99.0);

      const CorrectedCounters& c = stats.counters;

      print_info("--- Measured Phase Summary ---");
      print_info("lookups=" + std::to_string(lookup_count) +
                 ", throughput=" + std::to_string(stats.qps / 1e6) + " Mqps" +
                 ", elapsed=" + std::to_string(secs) + "s");
      print_info("Latency(us): avg=" + std::to_string(stats.avg_latency_us) +
                 ", p95=" + std::to_string(stats.p95_latency_us) +
                 ", p99=" + std::to_string(stats.p99_latency_us) +
                 ", samples=" + std::to_string(stats.latency_samples));
      print_info("RecordCache: hit=" + std::to_string(c.rc_hit) +
                 ", miss=" + std::to_string(c.rc_miss) +
                 ", RC_HR=" + std::to_string(c.rc_hr) + "%");
      print_info("Theory gap: RC_gap_to_theory=" +
                 std::to_string(std::max(0.0, theoretical_targets.rc_hr_upper_pct - c.rc_hr)) +
                 "pp, DRAM_gap_to_theory=" +
                 std::to_string(std::max(0.0, theoretical_targets.dram_page_hr_upper_pct - c.dram_hr)) +
                 "pp");
      print_info("DRAM(warm)=" + std::to_string(c.true_dram_hit) +
                 ", CXL="      + std::to_string(c.cxl_hit) +
                 ", SSD="      + std::to_string(c.ssd_miss) +
                 ", total="    + std::to_string(c.total));
      print_info("DRAM_HR="  + std::to_string(c.dram_hr)  + "%" +
                 ", CXL_HR=" + std::to_string(c.cxl_hr)   + "%" +
                 ", SSD_miss_rate=" + std::to_string(c.ssd_rate) + "%" +
                 "  [rc+dram+cxl+ssd=total, mutually exclusive]");
      print_info("CXL->DRAM promotions=" + std::to_string(c.promotions) +
                 ", evictions=" + std::to_string(c.evictions));
      print_info("RecordCache SIEVE evicted_entries=" + std::to_string(c.sieve_eviction_entries));
      print_info("TwoLevel decisions: full_page=" + std::to_string(c.decision_full_page) +
                 ", record_page=" + std::to_string(c.decision_record_page) +
                 ", record_slots=" + std::to_string(c.decision_record_slots));
      print_info("found=" + std::to_string(stats.found) +
                 ", not_found=" + std::to_string(stats.not_found) +
                 ", TX_aborts=" + std::to_string(stats.aborts));
   } else {
      print_info("Warmup done: " + std::to_string(lookup_count) + " lookups" +
                 ", throughput=" + std::to_string(lookup_count / secs / 1e6) + " Mqps");
      auto& bm = *storage::BMC::global_bf;
      if (bm.global_record_cache != nullptr) {
         const u64 rc_entries = bm.global_record_cache->getActiveEntryCount();
         const u64 rc_capacity = bm.global_record_cache->getLogicalCapacity();
         const double rc_fill_pct =
             (rc_capacity > 0) ? (100.0 * static_cast<double>(rc_entries) / static_cast<double>(rc_capacity)) : 0.0;
         print_info("[RC State] entry_count=" + std::to_string(rc_entries) +
                    ", capacity=" + std::to_string(rc_capacity) +
                    ", fill=" + std::to_string(rc_fill_pct) + "%");
      }
   }

   return stats;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv)
{
   gflags::SetUsageMessage(
       "YCSB-C (100% read) simulation for CXL record cache experiments");
   gflags::ParseCommandLineFlags(&argc, &argv, true);

   // Validate admission mode
   if (FLAGS_test_admission_mode != "lru" &&
       FLAGS_test_admission_mode != "page_only" &&
       FLAGS_test_admission_mode != "two_level") {
      print_fail("Unsupported --test_admission_mode; expected: lru | page_only | two_level");
      return 1;
   }

   // Propagate admission mode to LeanStore global flags
   FLAGS_admission_mode = FLAGS_test_admission_mode;
   if (FLAGS_test_admission_mode == "lru" || FLAGS_test_admission_mode == "page_only") {
      FLAGS_enable_record_cache         = false;
      FLAGS_dram_recordcache_gib        = 0.0;
      FLAGS_forward_epoch_thread        = 0;
      FLAGS_sieve_eviction_thread       = 0;
      FLAGS_record_cache_promote_thread = 0;
   }
   if (FLAGS_test_admission_mode == "lru") {
      FLAGS_two_level_admission_threads = 0;
   } else if (FLAGS_test_admission_mode == "page_only") {
      if (FLAGS_two_level_admission_threads == 0) {
         FLAGS_two_level_admission_threads = 1;
      }
   }

   if (!FLAGS_cxl_tiering_enabled) {
      print_warn("--cxl_tiering_enabled=false; this test targets the CXL tiering path");
   }

   const Config cfg = build_config_from_flags();
   const TheoreticalTargets theoretical_targets = build_theoretical_targets(cfg);

   if (cfg.records_per_page() == 0) {
      print_fail("records_per_page == 0; "
                 "check page_size_bytes / record_size_bytes / fill_factor");
      return 1;
   }
   if (cfg.total_records() < 2) {
      print_fail("total_records must be >= 2");
      return 1;
   }

   // Banner
   std::cout << "\n" << Color::BOLD << Color::CYAN
             << "============================================\n"
             << "  YCSB-C Test (100% Read, ScrambledZipf)\n"
             << "============================================"
             << Color::RESET << "\n";

   print_info("Configuration:");
   print_info("  admission_mode           = " + FLAGS_test_admission_mode);
   print_info("  working_set_gib          = " + std::to_string(cfg.working_set_gib));
   print_info("  record_cache_gib         = " + std::to_string(cfg.record_cache_gib));
   print_info("  zipf_theta               = " + std::to_string(cfg.zipf_theta));
   print_info("  fill_factor              = " + std::to_string(cfg.fill_factor));
   print_info("  payload_size_bytes       = " + std::to_string(cfg.payload_size_bytes) +
              " B  (key=8 B fixed)");
   print_info("  per_record_on_page_bytes = " + std::to_string(cfg.per_record_on_page_bytes()) +
              " B  (Slot=10 + key=8 + ChainedTuple=25 + payload=" +
              std::to_string(cfg.payload_size_bytes) + ")");
   print_info("  records_per_page         = " + std::to_string(cfg.records_per_page()));
   print_info("  total_pages              = " + std::to_string(cfg.total_pages()));
   print_info("  total_records            = " + std::to_string(cfg.total_records()));
   print_info("  rc_entry_bytes           = " + std::to_string(cfg.rc_entry_bytes()) +
              " B  (RCEntry=16 + key=8 + payload=" +
              std::to_string(cfg.payload_size_bytes) + ")");
   print_info("  record_cache_capacity    = " + std::to_string(cfg.record_cache_capacity()));
   print_info("  warmup_lookups           = " + std::to_string(FLAGS_test_warmup_lookups));
   print_info("  measure_lookups          = " + std::to_string(FLAGS_test_measure_lookups));
   print_info("  seed                     = " + std::to_string(cfg.seed));
   print_info("  counter_correction       = dram_hit - ssd_miss  (mode-independent)");
   print_info("  theoretical_RC_HR_upper  = " + std::to_string(theoretical_targets.rc_hr_upper_pct) + "%");
   print_info("  theoretical_DRAM_HR_upper= " + std::to_string(theoretical_targets.dram_page_hr_upper_pct) + "%");

   print_workload_sanity_report(cfg);

   // Build the ScrambledZipf generator once; reused across phases.
   ScrambledZipfGenerator generator(cfg.total_records(), cfg.zipf_theta);

   // Initialize LeanStore
   print_phase("Initialize LeanStore");
   LeanStore db;
   auto& crm = db.getCRManager();
   LeanStoreAdapter<YCSBTable> table;
   crm.scheduleJobSync(0, [&]() {
      table = LeanStoreAdapter<YCSBTable>(db, "YCSB_C_TEST");
   });
   print_pass(std::string("LeanStore initialized (") +
              (FLAGS_vi ? "BTreeVI" : "BTreeLL") + ")");

   // Phase 1: bulk load
   phase1_load_data(crm, table, cfg.total_records(), cfg.payload_size_bytes);

   // Deferred background thread start
   if (FLAGS_cxl_tiering_enabled && FLAGS_delay_admission_recordcache_threads_start) {
      print_info("Enabling deferred background threads (admission + RecordCache)...");
      storage::BMC::global_bf->enableAdmissionAndRecordCacheThreads();
      if (storage::BMC::global_bf->global_record_cache != nullptr) {
         storage::BMC::global_bf->global_record_cache->setLogicalCapacityFromEntrySize(cfg.rc_entry_bytes());
      }
      print_info("Deferred background threads enabled.");
   }

   // Reset diagnostics — load-phase I/O must not pollute lookup stats
   reset_diag_counters();

   // Inject dynamic per-page-visits ceiling derived from workload characteristics.
   // Must be called AFTER storage::BMC is initialized (LeanStore db constructor),
   // BEFORE warmup starts, so that Condition-1 in CheckAndPromote uses the tighter
   // min(FLAGS_max_per_page_visits, dynamic_ceiling) from the very first epoch.
   if (auto* wrapper = storage::BMC::global_bf->getAdmissionControlWrapper(); wrapper != nullptr) {
      wrapper->ConfigureDynamicMaxPerPageVisits(FLAGS_test_working_set_gib,
                                                FLAGS_test_zipf_theta);
   }

   // Phase 2: warmup
   double warmup_secs = 0.0;
   if (FLAGS_test_warmup_lookups > 0) {
      print_phase("Phase 2: Warmup Lookup");
      auto warmup_stats = run_lookup_phase(crm, table, cfg, theoretical_targets, generator,
                             FLAGS_test_warmup_lookups, /*measured=*/false);
      warmup_secs = warmup_stats.secs;
      // Reset again — warmup I/O must not pollute measured stats
      reset_diag_counters();
   }

   // Phase 3: measured YCSB-C run
   print_phase("Phase 3: Measured Lookup (YCSB-C, 100% Read)");
   const LookupRunStats stats = run_lookup_phase(
      crm, table, cfg, theoretical_targets, generator,
       FLAGS_test_measure_lookups, /*measured=*/true);

   // ============================================================================
   // Final Summary
   // ============================================================================
   // stats.counters was populated by read_and_correct_counters() inside
   // run_lookup_phase, so no re-reading or re-correction is needed here.
   // This is the single source of truth for the final numbers.
   // ============================================================================
   {
      const CorrectedCounters& c = stats.counters;
      const PromoteEfficiencyMetrics pe = compute_promote_efficiency_metrics(c);

      print_phase("Final Summary");

      // One-line grep-friendly summary
      print_info(
          "mode="       + FLAGS_test_admission_mode +
          ", QPS="      + std::to_string(stats.qps) +
          ", Mqps="     + std::to_string(stats.qps / 1e6) +
          ", p95_us="   + std::to_string(stats.p95_latency_us) +
          ", p99_us="   + std::to_string(stats.p99_latency_us) +
          ", avg_us="   + std::to_string(stats.avg_latency_us) +
          ", RC_HR="    + std::to_string(c.rc_hr)    + "%" +
          ", DRAM_HR="  + std::to_string(c.dram_hr)  + "%" +
          ", CXL_HR="   + std::to_string(c.cxl_hr)   + "%" +
          ", SSD_miss=" + std::to_string(c.ssd_rate)  + "%" +
          ", rc_hit="   + std::to_string(c.rc_hit) +
          ", dram_hit=" + std::to_string(c.true_dram_hit) +
          ", cxl_hit="  + std::to_string(c.cxl_hit) +
          ", ssd_miss=" + std::to_string(c.ssd_miss) +
          ", total="    + std::to_string(c.total) +
          ", useful_promote=" + std::to_string(pe.useful_promote) +
          ", wasted_promote=" + std::to_string(pe.wasted_promote) +
          ", found="    + std::to_string(stats.found) +
          ", not_found="+ std::to_string(stats.not_found) +
          ", aborts="   + std::to_string(stats.aborts));

      // Detailed breakdown
      print_info("--- Counter breakdown (mutually exclusive, sum=total) ---");
      print_info("  RecordCache hit  = " + std::to_string(c.rc_hit) +
                 "  (" + std::to_string(c.rc_hr) + "%)");
      print_info("  DRAM warm hit    = " + std::to_string(c.true_dram_hit) +
                 "  (" + std::to_string(c.dram_hr) + "%)" +
                 "  [= raw_dram_hit - ssd_miss correction]");
      print_info("  CXL hit          = " + std::to_string(c.cxl_hit) +
                 "  (" + std::to_string(c.cxl_hr) + "%)");
      print_info("  SSD miss         = " + std::to_string(c.ssd_miss) +
                 "  (" + std::to_string(c.ssd_rate) + "%)");
      print_info("  TOTAL            = " + std::to_string(c.total));
      print_info("  RC SIEVE evicted = " + std::to_string(c.sieve_eviction_entries));
      print_info("  Theory gap(rc/dram) = " +
                 std::to_string(std::max(0.0, theoretical_targets.rc_hr_upper_pct - c.rc_hr)) +
                 "pp / " +
                 std::to_string(std::max(0.0, theoretical_targets.dram_page_hr_upper_pct - c.dram_hr)) +
                 "pp");
   }

   if (FLAGS_test_admission_mode == "two_level") {
      auto& bm = *storage::BMC::global_bf;
      if (bm.global_record_cache != nullptr) {
         const u64 rc_entries = bm.global_record_cache->getActiveEntryCount();
         const u64 rc_capacity = bm.global_record_cache->getLogicalCapacity();
         const double rc_fill_pct =
             (rc_capacity > 0) ? (100.0 * static_cast<double>(rc_entries) / static_cast<double>(rc_capacity)) : 0.0;
         print_info("[RC State][Final] entry_count=" + std::to_string(rc_entries) +
                    ", capacity=" + std::to_string(rc_capacity) +
                    ", fill=" + std::to_string(rc_fill_pct) + "%");
      }
   }

   const double total_secs = warmup_secs + stats.secs;
   const double warmup_pct = (total_secs > 0.0) ? (100.0 * warmup_secs / total_secs) : 0.0;
   print_info("warmup_elapsed=" + std::to_string(warmup_secs) + "s" +
              ", measure_elapsed=" + std::to_string(stats.secs) + "s" +
              ", warmup_ratio=" + std::to_string(warmup_pct) + "%");

   print_pass("YCSB-C test finished.");
   return 0;
}