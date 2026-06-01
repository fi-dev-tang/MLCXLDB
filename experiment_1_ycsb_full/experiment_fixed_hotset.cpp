// =============================================================================
// experiment_fixed_hotset.cpp
//
// Ground-truth fixed hot-set test for the CXL record cache.
//
// Workload design (vs. ScrambledZipf):
//   - HOT_KEY_COUNT specific keys form a flat "hot set" (chosen as
//     fnvHash64(i) % total_records for i in [0, HOT_KEY_COUNT) -- spread
//     across pages so we exercise the slot-level admission path).
//   - HOT_RATIO of every access goes uniformly across the hot set.
//   - The rest goes uniformly over the entire key space.
//   - 100% reads (no updates), so cache contents are deterministic.
//
// What this gives us that Zipf cannot:
//   - Each hot key is hit (HOT_RATIO * lookups) / HOT_KEY_COUNT times. With
//     warmup=20M, hot_ratio=0.95, K=1024, that's ~18,500 hits per hot key
//     during warmup -- WAY above any reasonable admission threshold. So
//     these keys MUST end up in the RecordCache. If they don't, we have a
//     concrete bug to chase.
//   - Probe phase enumerates each hot key one-by-one and reads the per-tier
//     counter delta to determine where the lookup landed (RC / DRAM / CXL /
//     SSD). The output lists exactly which "should-be-in-cache" keys are
//     missing.
//
// Phase flow:
//   1. Bulk load
//   2. Warmup with FixedHotset workload
//   3. Probe every hot key once -> dump miss list
//   4. Measure with FixedHotset workload (optional, for QPS reference)
//   5. Probe again at the end
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <set>
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
DEFINE_double(test_working_set_gib,        1.0,            "Target working set size in GiB");
DEFINE_uint64(test_payload_size_bytes,     100,            "Payload bytes per record (50–200); key is always 8 B");
DEFINE_double(test_fill_factor,            0.5,            "B+Tree leaf page fill factor");
DEFINE_uint64(test_warmup_lookups,         20000000ULL,    "Warmup lookup count before probe phase");
DEFINE_uint64(test_measure_lookups,        5000000ULL,     "Measured lookup count after first probe (0 = skip measure phase)");
DEFINE_uint64(test_progress_interval,      500000ULL,      "Print progress every N lookups");
DEFINE_uint64(test_warmup_progress_interval, 500000ULL,    "Warmup progress interval");
DEFINE_uint64(test_seed,                   42ULL,          "Random seed");
DEFINE_string(test_admission_mode,         "two_level",    "Ablation mode: lru | page_only | two_level");

// Fixed-hotset specific flags
DEFINE_uint64(test_hot_key_count, 1024,  "Number of hot keys in the fixed hot set");
DEFINE_double(test_hot_ratio,     0.95,  "Fraction of accesses that go to the hot set");
DEFINE_uint64(test_probe_dump_max, 50,   "Max number of missing-hot-key entries to dump per probe");
// 0.0 = YCSB-C style (read-only); 0.05 = YCSB-B style (95% read, 5% update).
// When > 0, the update path goes through the same FixedHotsetGenerator, so
// updates target the hot set with HOT_RATIO probability — mirroring how
// YCSB-B updates also follow Zipf.
DEFINE_double(test_update_ratio,  0.0,   "Fraction of ops that are updates instead of reads");

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
   double      working_set_gib         = 1.0;
   double      record_cache_gib        = 0.05;
   u64         payload_size_bytes      = 100;
   u64         seed                    = 42ULL;
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
   cfg.payload_size_bytes  = FLAGS_test_payload_size_bytes;
   cfg.seed                = FLAGS_test_seed;
   cfg.admission_mode      = FLAGS_test_admission_mode;
   return cfg;
}

// ============================================================================
// Counter correction (mirrors experiment1_ycsb_b.cpp)
// dram_buffer_pool_hit double-counts SSD-cold-loads, so true_dram = raw - ssd.
// ============================================================================
struct CorrectedCounters {
   u64 rc_hit = 0, true_dram_hit = 0, cxl_hit = 0, ssd_miss = 0, total = 0;
   u64 sieve_eviction_entries = 0;
   double rc_hr = 0.0, dram_hr = 0.0, cxl_hr = 0.0, ssd_rate = 0.0;
};

static CorrectedCounters read_and_correct_counters() {
   auto& bm = *storage::BMC::global_bf;
   const u64 rc_h       = bm.diag.record_cache_hit.load(std::memory_order_relaxed);
   const u64 raw_dram_h = bm.diag.dram_buffer_pool_hit.load(std::memory_order_relaxed);
   const u64 cxl_h      = bm.diag.cxl_buffer_pool_hit.load(std::memory_order_relaxed);
   const u64 ssd_m      = bm.diag.ssd_miss.load(std::memory_order_relaxed);
   const u64 sieve_e    = (bm.global_record_cache != nullptr)
                          ? bm.global_record_cache->getSieveEvictionEntries() : 0;
   const u64 true_dram_h = (raw_dram_h >= ssd_m) ? (raw_dram_h - ssd_m) : 0;
   const u64 total       = rc_h + true_dram_h + cxl_h + ssd_m;

   CorrectedCounters c;
   c.rc_hit = rc_h; c.true_dram_hit = true_dram_h;
   c.cxl_hit = cxl_h; c.ssd_miss = ssd_m;
   c.total = total; c.sieve_eviction_entries = sieve_e;
   if (total > 0) {
      c.rc_hr    = 100.0 * static_cast<double>(rc_h)        / static_cast<double>(total);
      c.dram_hr  = 100.0 * static_cast<double>(true_dram_h) / static_cast<double>(total);
      c.cxl_hr   = 100.0 * static_cast<double>(cxl_h)       / static_cast<double>(total);
      c.ssd_rate = 100.0 * static_cast<double>(ssd_m)       / static_cast<double>(total);
   }
   return c;
}

static void reset_diag_counters() {
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
// SplitMix64 + FNV-1a
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

static u64 fnvHash64(u64 val) {
   constexpr u64 FNV_OFFSET_BASIS = 0xCBF29CE484222325ULL;
   constexpr u64 FNV_PRIME        = 1099511628211ULL;
   u64 h = FNV_OFFSET_BASIS;
   for (int i = 0; i < 8; i++) {
      h ^= (val & 0xFF);
      h *= FNV_PRIME;
      val >>= 8;
   }
   return h;
}

// ============================================================================
// FixedHotsetGenerator
//   - Hot set: HOT_KEY_COUNT unique keys in [0, total_records), spread by FNV
//   - HOT_RATIO of next() returns a uniformly-picked hot key
//   - (1-HOT_RATIO) returns a uniformly-picked key over the whole key space
// ============================================================================
class FixedHotsetGenerator {
public:
   FixedHotsetGenerator(u64 hot_key_count, u64 total_records, double hot_ratio)
       : total_records_(total_records), hot_ratio_(hot_ratio)
   {
      hot_keys_.reserve(hot_key_count);
      std::set<u64> seen;
      for (u64 i = 0; hot_keys_.size() < hot_key_count; i++) {
         const u64 k = fnvHash64(i + 1) % total_records_;
         if (seen.insert(k).second) hot_keys_.push_back(k);
         if (i > hot_key_count * 100ULL) break; // safety
      }
   }
   YCSBKey next(SplitMix64& rng) const {
      if (rng.next01() < hot_ratio_) {
         const u64 idx = rng.next() % hot_keys_.size();
         return hot_keys_[idx];
      }
      return rng.next() % total_records_;
   }
   const std::vector<u64>& hot_keys() const { return hot_keys_; }
   size_t hot_key_count() const { return hot_keys_.size(); }
private:
   std::vector<u64> hot_keys_;
   u64 total_records_;
   double hot_ratio_;
};

// ============================================================================
// Phase 1: Load
// ============================================================================
void phase1_load_data(cr::CRManager& crm,
                      LeanStoreAdapter<YCSBTable>& table,
                      u64 tuple_count, u64 payload_size_bytes)
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
                cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
                table.insert_var({i}, {payload}, payload_size_bytes);
                cr::Worker::my().commitTX();
             }
          });
       });
   crm.joinAll();
   const double secs = std::chrono::duration<double>(
       std::chrono::high_resolution_clock::now() - t0).count();
   print_info("Loaded " + std::to_string(tuple_count) + " records in " +
              std::to_string(secs) + " s");
}

// ============================================================================
// Workload phase: 100% read with FixedHotsetGenerator
// ============================================================================
struct PhaseStats {
   double secs = 0.0;
   u64 lookups = 0;
   u64 aborts  = 0;
   double qps  = 0.0;
};

static PhaseStats run_workload_phase(cr::CRManager& crm,
                                     LeanStoreAdapter<YCSBTable>& table,
                                     const FixedHotsetGenerator& gen,
                                     u64 lookup_count,
                                     bool is_warmup)
{
   std::atomic<u64> done{0};
   std::atomic<u64> aborts{0};

   auto t0 = std::chrono::high_resolution_clock::now();
   const u64 thread_count = static_cast<u64>(FLAGS_worker_threads);
   const u64 base_quota = lookup_count / thread_count;
   const u64 remainder  = lookup_count % thread_count;

   for (u64 t_i = 0; t_i < thread_count; t_i++) {
      const u64 quota = base_quota + (t_i < remainder ? 1 : 0);
      crm.scheduleJobAsync(t_i, [&, t_i, quota]() {
         const u64 phase_seed = FLAGS_test_seed ^
             (is_warmup ? 0x9E3779B97F4A7C15ULL : 0xD1B54A32D192ED03ULL);
         SplitMix64 rng(phase_seed + 0x9e3779b97f4a7c15ULL * (t_i + 1));
         volatile u64 local_done = 0;

         while (local_done < quota) {
            const auto rng_snapshot = rng;
            jumpmuTry() {
               const YCSBKey key = gen.next(rng);
               const double op_roll = rng.next01();
               const bool is_update = (op_roll < FLAGS_test_update_ratio);
               cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
               if (!is_update) {
                  table.lookup1({key}, [&](const YCSBTable&) {});
               } else {
                  UpdateDescriptorGenerator1(update_desc, YCSBTable, my_payload);
                  update_desc.slots[0].length = static_cast<u16>(FLAGS_test_payload_size_bytes);
                  YCSBPayload payload;
                  std::memset(payload.value, 0, sizeof(payload.value));
                  utils::RandomGenerator::getRandString(payload.value, FLAGS_test_payload_size_bytes);
                  table.update1({key}, [&](YCSBTable& rec) {
                     std::memcpy(rec.my_payload.value, payload.value, FLAGS_test_payload_size_bytes);
                  }, update_desc);
               }
               cr::Worker::my().commitTX();
               local_done++;

               const u64 global_done = done.fetch_add(1, std::memory_order_relaxed) + 1;
               const u64 progress_interval =
                   is_warmup ? FLAGS_test_warmup_progress_interval
                             : FLAGS_test_progress_interval;
               if (progress_interval > 0 && global_done % progress_interval == 0) {
                  const double elapsed = std::chrono::duration<double>(
                      std::chrono::high_resolution_clock::now() - t0).count();
                  const double cur_qps = (elapsed > 0.0)
                      ? (static_cast<double>(global_done) / elapsed) : 0.0;
                  const std::string tag = is_warmup ? "[Warmup]" : "[Measure]";
                  std::ostringstream oss;
                  oss << tag << " " << global_done << "/" << lookup_count
                      << " elapsed=" << elapsed << "s"
                      << " throughput=" << (cur_qps / 1e6) << " Mqps";
                  if (!is_warmup) {
                     auto c = read_and_correct_counters();
                     oss << " RC_HR=" << c.rc_hr << "%"
                         << " CXL_HR=" << c.cxl_hr << "%"
                         << " SSD_miss=" << c.ssd_rate << "%";
                  }
                  print_info(oss.str());
               }
            } jumpmuCatch() {
               rng = rng_snapshot;
               aborts.fetch_add(1, std::memory_order_relaxed);
            }
         }
      });
   }
   crm.joinAll();
   const double secs = std::chrono::duration<double>(
       std::chrono::high_resolution_clock::now() - t0).count();

   PhaseStats s;
   s.secs = secs;
   s.lookups = lookup_count;
   s.aborts = aborts.load(std::memory_order_relaxed);
   s.qps = (secs > 0.0) ? (static_cast<double>(lookup_count) / secs) : 0.0;
   return s;
}

// ============================================================================
// Probe phase: enumerate every hot key, single-threaded, classify per-tier
// ============================================================================
struct ProbeResult {
   u64 rc_count   = 0;
   u64 dram_count = 0;
   u64 cxl_count  = 0;
   u64 ssd_count  = 0;
   u64 unknown    = 0;
   std::vector<std::pair<u64,std::string>> not_in_rc; // (key, tier)
};

static ProbeResult probe_hot_set(cr::CRManager& crm,
                                 LeanStoreAdapter<YCSBTable>& table,
                                 const FixedHotsetGenerator& gen,
                                 const std::string& probe_label)
{
   print_phase("Probe Hot Set: " + probe_label);
   ProbeResult result;
   const auto& hot_keys = gen.hot_keys();
   auto& bm = *storage::BMC::global_bf;

   const auto t0 = std::chrono::high_resolution_clock::now();
   for (size_t i = 0; i < hot_keys.size(); i++) {
      const u64 key = hot_keys[i];
      const u64 rc_b = bm.diag.record_cache_hit.load(std::memory_order_relaxed);
      const u64 dr_b = bm.diag.dram_buffer_pool_hit.load(std::memory_order_relaxed);
      const u64 cx_b = bm.diag.cxl_buffer_pool_hit.load(std::memory_order_relaxed);
      const u64 ss_b = bm.diag.ssd_miss.load(std::memory_order_relaxed);

      crm.scheduleJobSync(0, [&]() {
         jumpmuTry() {
            cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
            table.lookup1({key}, [&](const YCSBTable&) {});
            cr::Worker::my().commitTX();
         } jumpmuCatch() {}
      });

      const u64 rc_a = bm.diag.record_cache_hit.load(std::memory_order_relaxed);
      const u64 dr_a = bm.diag.dram_buffer_pool_hit.load(std::memory_order_relaxed);
      const u64 cx_a = bm.diag.cxl_buffer_pool_hit.load(std::memory_order_relaxed);
      const u64 ss_a = bm.diag.ssd_miss.load(std::memory_order_relaxed);

      const bool rc_hit  = (rc_a > rc_b);
      const bool ssd_hit = (ss_a > ss_b);
      const bool cxl_hit = (cx_a > cx_b);
      const bool dr_hit  = (dr_a > dr_b);

      // Priority: RC > SSD > CXL > DRAM (because SSD load increments dram too)
      std::string tier;
      if (rc_hit) { result.rc_count++; tier = "RC"; }
      else if (ssd_hit)  { result.ssd_count++;  tier = "SSD";  }
      else if (cxl_hit)  { result.cxl_count++;  tier = "CXL";  }
      else if (dr_hit)   { result.dram_count++; tier = "DRAM"; }
      else               { result.unknown++;    tier = "?";    }

      if (!rc_hit && result.not_in_rc.size() < FLAGS_test_probe_dump_max) {
         result.not_in_rc.emplace_back(key, tier);
      }
   }
   const double secs = std::chrono::duration<double>(
       std::chrono::high_resolution_clock::now() - t0).count();

   const double total = static_cast<double>(hot_keys.size());
   print_info("PROBE_SUMMARY label=" + probe_label +
              " hot_set=" + std::to_string(hot_keys.size()) +
              " RC="    + std::to_string(result.rc_count)   +
              " (" + std::to_string(100.0 * result.rc_count   / total) + "%)" +
              " DRAM="  + std::to_string(result.dram_count) +
              " (" + std::to_string(100.0 * result.dram_count / total) + "%)" +
              " CXL="   + std::to_string(result.cxl_count)  +
              " (" + std::to_string(100.0 * result.cxl_count  / total) + "%)" +
              " SSD="   + std::to_string(result.ssd_count)  +
              " (" + std::to_string(100.0 * result.ssd_count  / total) + "%)" +
              " UNK="   + std::to_string(result.unknown) +
              " elapsed=" + std::to_string(secs) + "s");

   if (!result.not_in_rc.empty()) {
      print_info("PROBE_MISSING (first " +
                 std::to_string(result.not_in_rc.size()) +
                 " hot keys NOT in RC):");
      const u64 N = result.not_in_rc.size();
      const u64 per_line = 8;
      for (u64 i = 0; i < N; i += per_line) {
         std::ostringstream oss;
         oss << "  ";
         for (u64 j = i; j < std::min<u64>(i + per_line, N); j++) {
            oss << "(" << result.not_in_rc[j].first << "," << result.not_in_rc[j].second << ") ";
         }
         std::cout << oss.str() << "\n";
      }
   }
   return result;
}

// ============================================================================
int main(int argc, char** argv)
{
   gflags::SetUsageMessage("Fixed hot-set ground-truth test for CXL record cache");
   gflags::ParseCommandLineFlags(&argc, &argv, true);

   if (FLAGS_test_admission_mode != "lru" &&
       FLAGS_test_admission_mode != "page_only" &&
       FLAGS_test_admission_mode != "two_level") {
      print_fail("Unsupported --test_admission_mode");
      return 1;
   }
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
   } else if (FLAGS_test_admission_mode == "page_only" &&
              FLAGS_two_level_admission_threads == 0) {
      FLAGS_two_level_admission_threads = 1;
   }
   if (!FLAGS_cxl_tiering_enabled) {
      print_warn("--cxl_tiering_enabled=false; this test targets the CXL tiering path");
   }

   const Config cfg = build_config_from_flags();

   std::cout << "\n" << Color::BOLD << Color::CYAN
             << "============================================\n"
             << "  Fixed Hot-Set Ground-Truth Test\n"
             << "============================================" << Color::RESET << "\n";

   print_info("admission_mode        = " + FLAGS_test_admission_mode);
   print_info("working_set_gib       = " + std::to_string(cfg.working_set_gib));
   print_info("record_cache_gib      = " + std::to_string(cfg.record_cache_gib));
   print_info("payload_size_bytes    = " + std::to_string(cfg.payload_size_bytes));
   print_info("records_per_page      = " + std::to_string(cfg.records_per_page()));
   print_info("total_pages           = " + std::to_string(cfg.total_pages()));
   print_info("total_records         = " + std::to_string(cfg.total_records()));
   print_info("rc_entry_bytes        = " + std::to_string(cfg.rc_entry_bytes()));
   print_info("record_cache_capacity = " + std::to_string(cfg.record_cache_capacity()));
   print_info("hot_key_count         = " + std::to_string(FLAGS_test_hot_key_count));
   print_info("hot_ratio             = " + std::to_string(FLAGS_test_hot_ratio));
   print_info("update_ratio          = " + std::to_string(FLAGS_test_update_ratio) +
              "  (0.0 = YCSB-C style, 0.05 = YCSB-B style)");
   print_info("warmup_lookups        = " + std::to_string(FLAGS_test_warmup_lookups));
   print_info("measure_lookups       = " + std::to_string(FLAGS_test_measure_lookups));

   // Sanity: hot_key_count must fit in record cache (else test is meaningless)
   if (FLAGS_test_admission_mode == "two_level" &&
       FLAGS_test_hot_key_count > cfg.record_cache_capacity()) {
      print_warn("hot_key_count (" + std::to_string(FLAGS_test_hot_key_count) +
                 ") > record_cache_capacity (" + std::to_string(cfg.record_cache_capacity()) +
                 "). Hot set cannot fully fit in cache.");
   }
   const double per_hot_key_warmup_hits =
       static_cast<double>(FLAGS_test_warmup_lookups) * FLAGS_test_hot_ratio /
       static_cast<double>(FLAGS_test_hot_key_count);
   print_info("expected per-hot-key warmup hits ≈ " +
              std::to_string(per_hot_key_warmup_hits));

   // Build generator
   FixedHotsetGenerator generator(FLAGS_test_hot_key_count, cfg.total_records(),
                                  FLAGS_test_hot_ratio);
   print_info("hot_set actually built: " + std::to_string(generator.hot_key_count()) + " unique keys");

   // Init
   print_phase("Initialize LeanStore");
   LeanStore db;
   auto& crm = db.getCRManager();
   LeanStoreAdapter<YCSBTable> table;
   crm.scheduleJobSync(0, [&]() {
      table = LeanStoreAdapter<YCSBTable>(db, "FIXED_HOTSET");
   });
   print_pass(std::string("LeanStore initialized (") +
              (FLAGS_vi ? "BTreeVI" : "BTreeLL") + ")");

   // Load
   phase1_load_data(crm, table, cfg.total_records(), cfg.payload_size_bytes);

   // Deferred admission/RC threads
   if (FLAGS_cxl_tiering_enabled && FLAGS_delay_admission_recordcache_threads_start) {
      print_info("Enabling deferred background threads (admission + RecordCache)...");
      storage::BMC::global_bf->enableAdmissionAndRecordCacheThreads();
      if (storage::BMC::global_bf->global_record_cache != nullptr) {
         storage::BMC::global_bf->global_record_cache
             ->setLogicalCapacityFromEntrySize(cfg.rc_entry_bytes());
      }
      print_info("Deferred background threads enabled.");
   }

   reset_diag_counters();

   // Inject dynamic threshold (zipf_theta unused in fixed-hotset; pass 0.9 placeholder)
   if (auto* wrapper = storage::BMC::global_bf->getAdmissionControlWrapper(); wrapper != nullptr) {
      wrapper->ConfigureDynamicMaxPerPageVisits(FLAGS_test_working_set_gib, 0.9);
   }

   // Phase 2: Warmup
   double warmup_secs = 0.0;
   if (FLAGS_test_warmup_lookups > 0) {
      print_phase("Phase 2: Warmup");
      auto warmup_stats = run_workload_phase(crm, table, generator,
                                             FLAGS_test_warmup_lookups, /*is_warmup=*/true);
      warmup_secs = warmup_stats.secs;
      print_info("warmup elapsed=" + std::to_string(warmup_stats.secs) + "s, " +
                 "QPS=" + std::to_string(warmup_stats.qps / 1e6) + " Mqps, " +
                 "aborts=" + std::to_string(warmup_stats.aborts));
   }

   // Probe immediately after warmup
   ProbeResult probe1 = probe_hot_set(crm, table, generator, "after_warmup");

   // Phase 3: Measure (optional)
   PhaseStats measure_stats{};
   reset_diag_counters();
   if (FLAGS_test_measure_lookups > 0) {
      print_phase("Phase 3: Measure");
      measure_stats = run_workload_phase(crm, table, generator,
                                         FLAGS_test_measure_lookups, /*is_warmup=*/false);
      print_info("measure elapsed=" + std::to_string(measure_stats.secs) + "s, " +
                 "QPS=" + std::to_string(measure_stats.qps / 1e6) + " Mqps, " +
                 "aborts=" + std::to_string(measure_stats.aborts));
      const auto c = read_and_correct_counters();
      print_info("MEASURE_FINAL RC_HR=" + std::to_string(c.rc_hr) +
                 "% DRAM_HR=" + std::to_string(c.dram_hr) +
                 "% CXL_HR=" + std::to_string(c.cxl_hr) +
                 "% SSD_miss=" + std::to_string(c.ssd_rate) +
                 "% total=" + std::to_string(c.total));
   }

   // Probe again after measure
   ProbeResult probe2 = probe_hot_set(crm, table, generator, "after_measure");

   // RC final state
   if (FLAGS_test_admission_mode == "two_level") {
      auto& bm = *storage::BMC::global_bf;
      if (bm.global_record_cache != nullptr) {
         const u64 e = bm.global_record_cache->getActiveEntryCount();
         const u64 cap = bm.global_record_cache->getLogicalCapacity();
         const double fill = (cap > 0) ? (100.0 * static_cast<double>(e) / cap) : 0.0;
         print_info("[RC State][Final] entry_count=" + std::to_string(e) +
                    " capacity=" + std::to_string(cap) +
                    " fill=" + std::to_string(fill) + "%");
      }
   }

   // Compact summary
   print_phase("Final Summary");
   const u64 hot_n = generator.hot_key_count();
   auto pct = [&](u64 x){ return std::to_string(100.0 * static_cast<double>(x) / hot_n); };
   print_info("HOT_SET_PROBE_SUMMARY hot_n=" + std::to_string(hot_n) +
              " probe1[RC=" + pct(probe1.rc_count) +
              "% CXL=" + pct(probe1.cxl_count) +
              "% DRAM=" + pct(probe1.dram_count) +
              "% SSD=" + pct(probe1.ssd_count) + "%]" +
              " probe2[RC=" + pct(probe2.rc_count) +
              "% CXL=" + pct(probe2.cxl_count) +
              "% DRAM=" + pct(probe2.dram_count) +
              "% SSD=" + pct(probe2.ssd_count) + "%]");

   // Operational counters from Plan B / C (kept; minimal one-line summary).
   if (auto& bm = *storage::BMC::global_bf;
       FLAGS_test_admission_mode == "two_level" && bm.global_record_cache != nullptr) {
      auto* rc = bm.global_record_cache;
      print_info("OPS rejected_high_water=" + std::to_string(rc->GetPromoteRejectedHighWater()) +
                 " pending_state_invalidated_from_hash_now=" + std::to_string(rc->GetPendingStateInvalidatedFromHash()) +
                 " sieve_eviction_entries=" + std::to_string(rc->getSieveEvictionEntries()));
   }

   print_pass("Fixed hot-set test finished.");
   return 0;
}
