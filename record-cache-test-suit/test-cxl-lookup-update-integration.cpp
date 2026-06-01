// =============================================================================
// test-cxl-lookup-update-integration.cpp
//
// Integration test for CXL-tiered mixed workload path:
//   Phase 1: Data loading (uniform insert)
//   Phase 2: CXL memory verification + DRAM/CXL bandwidth/latency
//   Phase 3: Two-Level-Zipfian mixed workload (lookup/update ratio configurable)
//   Phase 4: Hit-rate + update stats + admission control monitoring
//   Phase 5: Partition debug dump
// =============================================================================
#include "../frontend/shared/Adapter.hpp"
#include "../frontend/shared/LeanStoreAdapter.hpp"
#include "../frontend/shared/Schema.hpp"
#include "Units.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/storage/buffer-manager/BufferManager.hpp"
#include "leanstore/storage/two-level-admission-control/TwoLevelAdmissionControl.hpp"
#include "leanstore/utils/Parallelize.hpp"
#include "leanstore/utils/RandomGenerator.hpp"

#include <gflags/gflags.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <immintrin.h>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

DEFINE_uint64(test_seed, 42, "Random seed for lookup/update key generation");
DEFINE_uint64(test_tuple_count, 0, "Total records to insert (0 = auto from target_gib)");
DEFINE_double(test_data_gib, 4.0, "Target data size in GiB");
DEFINE_uint64(test_total_ops, 300000000ULL, "Total mixed operations (lookup+update)");
DEFINE_uint64(test_print_interval, 10000, "Print stats every N operations");
DEFINE_double(test_theta_page, 0.99, "Zipfian theta for inter-page skew");
DEFINE_double(test_theta_slot, 1.3, "Zipfian theta for intra-page skew");
DEFINE_uint64(test_records_per_page, 175, "Estimated records per BTree leaf page");
DEFINE_string(test_lookup_update_ratios, "80,50", "Comma-separated lookup ratios, e.g., 80,50");
DEFINE_string(test_admission_log_prefix, "admission_control_mix", "Prefix for admission stats CSV");
DEFINE_string(test_hit_stats_log_prefix, "hit_stats_mix", "Prefix for hit/update stats CSV");

using namespace leanstore;
using TestKey = u64;
using TestPayload = BytesPayload<8>;
using KVTable = Relation<TestKey, TestPayload>;

namespace Color {
const char* RESET = "\033[0m";
const char* RED = "\033[31m";
const char* GREEN = "\033[32m";
const char* YELLOW = "\033[33m";
const char* MAGENTA = "\033[35m";
const char* CYAN = "\033[36m";
const char* BOLD = "\033[1m";
}  // namespace Color

void print_pass(const std::string& msg) { std::cout << Color::GREEN << "[PASS] " << Color::RESET << msg << "\n"; }
void print_fail(const std::string& msg) { std::cout << Color::RED << "[FAIL] " << Color::RESET << msg << "\n"; }
void print_info(const std::string& msg) { std::cout << Color::CYAN << "[INFO] " << Color::RESET << msg << "\n"; }
void print_warn(const std::string& msg) { std::cout << Color::YELLOW << "[WARN] " << Color::RESET << msg << "\n"; }
void print_phase(const std::string& msg)
{
   std::cout << "\n" << Color::BOLD << Color::MAGENTA
             << "========================================\n"
             << "  " << msg << "\n"
             << "========================================" << Color::RESET << "\n";
}

class ZipfianSampler {
   std::vector<double> cdf_;

  public:
   void init(uint64_t n, double theta)
   {
      cdf_.resize(n);
      double sum = 0;
      for (uint64_t i = 0; i < n; i++) {
         sum += 1.0 / std::pow(static_cast<double>(i + 1), theta);
         cdf_[i] = sum;
      }
      double inv = 1.0 / sum;
      for (uint64_t i = 0; i < n; i++) {
         cdf_[i] *= inv;
      }
   }
   uint64_t sample(std::mt19937_64& rng) const
   {
      std::uniform_real_distribution<double> u(0.0, 1.0);
      double r = u(rng);
      auto it = std::lower_bound(cdf_.begin(), cdf_.end(), r);
      return static_cast<uint64_t>(std::distance(cdf_.begin(), it));
   }
};

class TwoLevelZipfianGenerator {
   ZipfianSampler page_sampler_;
   ZipfianSampler slot_sampler_;
   uint64_t records_per_page_ = 1;
   uint64_t total_keys_ = 1;

  public:
   void init(uint64_t num_pages, uint64_t records_per_page, double theta_page, double theta_slot)
   {
      records_per_page_ = records_per_page;
      total_keys_ = num_pages * records_per_page;
      print_info("Building page-level Zipfian CDF (" + std::to_string(num_pages) + ", theta=" + std::to_string(theta_page) + ")");
      page_sampler_.init(num_pages, theta_page);
      print_info("Building slot-level Zipfian CDF (" + std::to_string(records_per_page) + ", theta=" + std::to_string(theta_slot) + ")");
      slot_sampler_.init(records_per_page, theta_slot);
   }
   uint64_t next(std::mt19937_64& rng) const
   {
      uint64_t page = page_sampler_.sample(rng);
      uint64_t slot = slot_sampler_.sample(rng);
      uint64_t key = page * records_per_page_ + slot;
      return std::min(key, total_keys_ - 1);
   }
};

std::vector<u64> parse_lookup_ratios(const std::string& s)
{
   std::vector<u64> ratios;
   std::stringstream ss(s);
   std::string token;
   while (std::getline(ss, token, ',')) {
      if (token.empty()) {
         continue;
      }
      u64 v = std::stoull(token);
      if (v > 100) {
         v = 100;
      }
      ratios.push_back(v);
   }
   if (ratios.empty()) {
      ratios.push_back(80);
   }
   return ratios;
}

void phase1_load_data(cr::CRManager& crm, LeanStoreAdapter<KVTable>& table, u64 tuple_count)
{
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
                   << "Worker 0: " << pct << "%, pages=" << used_pages << " (" << used_gib << " GiB)";
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
   print_info("Load complete: " + std::to_string(tuple_count) + " records, " + std::to_string(final_pages) + " pages (" +
              std::to_string(final_gib) + " GiB), " + std::to_string(secs) + " sec, " + std::to_string(tuple_count / secs / 1e6) + " Mtps");
}

void phase2_cxl_verification_and_bandwidth()
{
   print_phase("Phase 2: CXL Memory Verification + Bandwidth Measurement");
   auto& bm = *storage::BMC::global_bf;
   storage::BufferFrame* dram_bfs = bm.getDRAMBFs();
   storage::BufferFrame* cxl_bfs = bm.getCXLBFs();
   u64 dram_num = bm.getPoolSize();
   u64 cxl_num = bm.getCXLPoolSize();

   u64 dram_active = 0, cxl_active = 0;
   for (u64 i = 0; i < dram_num; i++) {
      if (dram_bfs[i].header.state != storage::BufferFrame::STATE::FREE) {
         dram_active++;
      }
   }
   if (cxl_bfs && cxl_num > 0) {
      for (u64 i = 0; i < cxl_num; i++) {
         if (cxl_bfs[i].header.state != storage::BufferFrame::STATE::FREE) {
            cxl_active++;
         }
      }
   }

   print_info("DRAM BufferFrames: total=" + std::to_string(dram_num) + ", active=" + std::to_string(dram_active) +
              " (" + std::to_string(100.0 * dram_active / dram_num) + "%)");
   if (cxl_bfs && cxl_num > 0) {
      print_info("CXL  BufferFrames: total=" + std::to_string(cxl_num) + ", active=" + std::to_string(cxl_active) +
                 " (" + std::to_string(100.0 * cxl_active / cxl_num) + "%)");
      if (cxl_active > 0) {
         print_pass("CXL memory IS being used (" + std::to_string(cxl_active) + " active frames)");
      } else {
         print_warn("CXL memory has 0 active frames");
      }
   }

   auto flush_region = [](const void* ptr, u64 bytes) {
      const char* p = reinterpret_cast<const char*>(ptr);
      for (u64 i = 0; i < bytes; i += 64) {
         _mm_clflush(p + i);
      }
      _mm_mfence();
   };

   constexpr u64 LLC_FLUSH_THRESHOLD = 256ULL * 1024 * 1024;
   auto measure_seq_bw = [&flush_region](const char* label, storage::BufferFrame* pool, u64 count) -> double {
      if (!pool || count == 0) {
         return -1.0;
      }
      const u64 total_bytes = count * sizeof(storage::BufferFrame);
      const u64 cls_per_frame = sizeof(storage::BufferFrame) / 64;
      if (total_bytes <= LLC_FLUSH_THRESHOLD) {
         flush_region(pool, total_bytes);
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
      double gib = total_bytes / (1024.0 * 1024.0 * 1024.0);
      double bw = gib / secs;
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(2) << label << " seq full-frame scan: " << count << " frames, " << gib << " GiB, "
          << secs * 1000.0 << " ms, " << bw << " GiB/s";
      print_info(oss.str());
      (void)sink;
      return bw;
   };

   auto measure_rand_lat = [](const char* label, storage::BufferFrame* pool, u64 count, u64 samples) -> double {
      if (!pool || count == 0) {
         return -1.0;
      }
      std::mt19937_64 rng(12345);
      std::uniform_int_distribution<u64> dist(0, count - 1);
      std::vector<u64> indices(samples);
      for (auto& idx : indices) {
         idx = dist(rng);
      }
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
      double avg_ns = total_ns / samples;
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(1) << label << " random access: " << samples << " samples, avg=" << avg_ns << " ns/access";
      print_info(oss.str());
      (void)sink;
      return avg_ns;
   };

   const double dram_bw = measure_seq_bw("DRAM", dram_bfs, dram_num);
   const double cxl_bw = measure_seq_bw("CXL ", cxl_bfs, cxl_num);
   const double dram_lat = measure_rand_lat("DRAM", dram_bfs, dram_num, 100000);
   const double cxl_lat = measure_rand_lat("CXL ", cxl_bfs, cxl_num, 100000);

   auto ratio = [](double a, double b) {
      if (a <= 0 || b <= 0) return std::string("N/A");
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(2) << (a / b) << "x";
      return oss.str();
   };
   std::cout << "\n  Metric                              DRAM             CXL       Ratio\n";
   std::cout << "  --------------------------------------------------------------------\n";
   std::cout << std::left << std::setw(34) << "  Seq BW (GiB/s)" << std::right << std::setw(12) << std::fixed << std::setprecision(2)
             << dram_bw << std::setw(15) << cxl_bw << std::setw(12) << ratio(dram_bw, cxl_bw) << "\n";
   std::cout << std::left << std::setw(34) << "  Rand Lat (ns)" << std::right << std::setw(12) << std::fixed << std::setprecision(1)
             << dram_lat << std::setw(15) << cxl_lat << std::setw(12) << ratio(cxl_lat, dram_lat) << "\n";
}

void run_mixed_ratio(cr::CRManager& crm,
                     LeanStoreAdapter<KVTable>& table,
                     const TwoLevelZipfianGenerator& zipf_gen,
                     u64 total_ops,
                     u64 lookup_ratio_pct)
{
   const u64 update_ratio_pct = 100 - lookup_ratio_pct;
   print_phase("Phase 3+4: Mixed Workload lookup/update=" + std::to_string(lookup_ratio_pct) + "/" + std::to_string(update_ratio_pct));

   auto& bm = *storage::BMC::global_bf;
   bm.diag.record_cache_hit.store(0);
   bm.diag.record_cache_miss.store(0);
   bm.diag.dram_buffer_pool_hit.store(0);
   bm.diag.cxl_buffer_pool_hit.store(0);

   std::string ratio_tag = std::to_string(lookup_ratio_pct) + "_" + std::to_string(update_ratio_pct);
   std::ofstream admission_csv(FLAGS_test_admission_log_prefix + "_" + ratio_tag + ".csv", std::ios::trunc);
   admission_csv << "op_count,lookup_ratio,update_ratio,threshold_coarse,threshold_fine,candidates,global_requests\n";
   std::ofstream hit_csv(FLAGS_test_hit_stats_log_prefix + "_" + ratio_tag + ".csv", std::ios::trunc);
   hit_csv << "op_count,record_cache_hit,record_cache_miss,dram_buffer_pool_hit,cxl_buffer_pool_hit,update_commit,update_abort\n";

   std::atomic<u64> global_op_counter{0};
   std::atomic<u64> lookup_commit{0};
   std::atomic<u64> lookup_abort{0};
   std::atomic<u64> update_commit{0};
   std::atomic<u64> update_abort{0};
   std::atomic<bool> keep_running{true};

   const u64 ops_per_thread = total_ops / FLAGS_worker_threads;
   const u64 print_interval = FLAGS_test_print_interval;
   auto t_begin = std::chrono::high_resolution_clock::now();

   std::thread monitor([&]() {
      u64 last_printed = 0;
      while (keep_running.load(std::memory_order_relaxed)) {
         u64 current = global_op_counter.load(std::memory_order_relaxed);
         u64 intervals_now = current / print_interval;
         u64 intervals_last = last_printed / print_interval;
         if (intervals_now > intervals_last) {
            last_printed = intervals_now * print_interval;
            auto* wrapper = bm.getAdmissionControlWrapper();
            u64 th_coarse = 0, th_fine = 0, num_cand = 0, reqs = 0;
            if (wrapper) {
               th_coarse = wrapper->GetHistogram().GetAdmissionThreshold_coarse();
               th_fine = wrapper->GetHistogram().GetAdmissionThreshold_fine();
               num_cand = wrapper->GetHotPageCandidates().GetCandidatesSize();
               reqs = wrapper->GetHotPageCandidates().GetGlobalRequests();
            }
            admission_csv << last_printed << "," << lookup_ratio_pct << "," << update_ratio_pct << "," << th_coarse << "," << th_fine
                         << "," << num_cand << "," << reqs << "\n";
            hit_csv << last_printed << "," << bm.diag.record_cache_hit.load() << "," << bm.diag.record_cache_miss.load() << ","
                    << bm.diag.dram_buffer_pool_hit.load() << "," << bm.diag.cxl_buffer_pool_hit.load() << "," << update_commit.load()
                    << "," << update_abort.load() << "\n";
            if (last_printed % 1000000 == 0) {
               double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t_begin).count();
               std::ostringstream oss;
               oss << std::fixed << std::setprecision(2) << "[" << last_printed / 1000000 << "M] " << (last_printed / elapsed / 1e6)
                   << " Mtps | ratio L/U=" << lookup_ratio_pct << "/" << update_ratio_pct
                   << " | rc_hit=" << bm.diag.record_cache_hit.load() << " DRAM hit=" << bm.diag.dram_buffer_pool_hit.load()
                   << " CXL hit=" << bm.diag.cxl_buffer_pool_hit.load() << " upd_commit=" << update_commit.load() << " upd_abort="
                   << update_abort.load() << " th=" << th_fine << " cand=" << num_cand;
               print_info(oss.str());
            }
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
   });

   for (u64 t_i = 0; t_i < FLAGS_worker_threads; t_i++) {
      crm.scheduleJobAsync(t_i, [&, t_i]() {
         std::mt19937_64 rng(t_i * 11400714819323198485ULL + FLAGS_test_seed);
         std::uniform_int_distribution<u64> ratio_dist(0, 99);
         UpdateDescriptorGenerator1(update_desc, KVTable, my_payload);
         for (u64 i = 0; i < ops_per_thread; i++) {
            TestKey key = zipf_gen.next(rng);
            bool do_lookup = ratio_dist(rng) < lookup_ratio_pct;
            jumpmuTry()
            {
               cr::Worker::my().startTX(TX_MODE::OLTP, TX_ISOLATION_LEVEL::SNAPSHOT_ISOLATION);
               if (do_lookup) {
                  table.lookup1({key}, [&](const KVTable&) {});
                  lookup_commit.fetch_add(1, std::memory_order_relaxed);
               } else {
                  KVTable payload;
                  utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(KVTable));
                  table.update1({key}, [&](KVTable& rec) { rec = payload; }, update_desc);
                  update_commit.fetch_add(1, std::memory_order_relaxed);
               }
               cr::Worker::my().commitTX();
               global_op_counter.fetch_add(1, std::memory_order_relaxed);
            }
            jumpmuCatch()
            {
               if (do_lookup) {
                  lookup_abort.fetch_add(1, std::memory_order_relaxed);
               } else {
                  update_abort.fetch_add(1, std::memory_order_relaxed);
               }
               WorkerCounters::myCounters().tx_abort++;
            }
         }
      });
   }
   crm.joinAll();
   keep_running.store(false, std::memory_order_release);
   monitor.join();
   admission_csv.close();
   hit_csv.close();

   double secs = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t_begin).count();
   u64 actual_ops = global_op_counter.load();
   print_info("Mixed workload complete: ops=" + std::to_string(actual_ops) + ", elapsed=" + std::to_string(secs) + " sec, Mtps=" +
              std::to_string(actual_ops / secs / 1e6));
   print_info("lookup commit/abort = " + std::to_string(lookup_commit.load()) + "/" + std::to_string(lookup_abort.load()));
   print_info("update commit/abort = " + std::to_string(update_commit.load()) + "/" + std::to_string(update_abort.load()));
   print_info("RecordCache hit/miss = " + std::to_string(bm.diag.record_cache_hit.load()) + "/" + std::to_string(bm.diag.record_cache_miss.load()));
   print_info("DRAM/CXL btree hit = " + std::to_string(bm.diag.dram_buffer_pool_hit.load()) + "/" + std::to_string(bm.diag.cxl_buffer_pool_hit.load()));
}

void phase5_partition_dump()
{
   print_phase("Phase 5: Partition Debug Dump");
   auto& bm = *storage::BMC::global_bf;
   u64 dram_parts = bm.getDRAMPartitionsCount();
   u64 cxl_parts = bm.getCXLPartitionsCount();
   auto& parts = bm.getPartitions();
   print_info("Partition layout: DRAM=" + std::to_string(dram_parts) + ", CXL=" + std::to_string(cxl_parts) +
              ", total=" + std::to_string(bm.getTotalPartitionsCount()));

   u64 dram_free_total = 0, cxl_free_total = 0;
   for (u64 i = 0; i < dram_parts; i++) {
      dram_free_total += parts[i]->dram_free_list.counter.load();
   }
   for (u64 i = 0; i < cxl_parts; i++) {
      cxl_free_total += parts[dram_parts + i]->cxl_free_list.counter.load();
   }
   print_info("DRAM free BFs: " + std::to_string(dram_free_total) + " / " + std::to_string(bm.getPoolSize()));
   print_info("CXL  free BFs: " + std::to_string(cxl_free_total) + " / " + std::to_string(bm.getCXLPoolSize()));
}

int main(int argc, char** argv)
{
   gflags::SetUsageMessage("CXL Lookup+Update Integration Test");
   gflags::ParseCommandLineFlags(&argc, &argv, true);

   std::cout << "\n" << Color::BOLD << Color::CYAN
             << "============================================\n"
             << "  CXL Lookup+Update Integration Test\n"
             << "============================================" << Color::RESET << "\n";
   print_info("cxl_tiering_enabled  = " + std::to_string(FLAGS_cxl_tiering_enabled));
   print_info("dram_buffer_pool_gib = " + std::to_string(FLAGS_dram_buffer_pool_gib));
   print_info("dram_recordcache_gib = " + std::to_string(FLAGS_dram_recordcache_gib));
   print_info("cxl_gib              = " + std::to_string(FLAGS_cxl_gib));
   print_info("vi (BTreeVI)         = " + std::to_string(FLAGS_vi));
   print_info("worker_threads       = " + std::to_string(FLAGS_worker_threads));
   print_info("test_data_gib        = " + std::to_string(FLAGS_test_data_gib));
   print_info("test_total_ops       = " + std::to_string(FLAGS_test_total_ops));
   print_info("test_lookup_update_ratios = " + FLAGS_test_lookup_update_ratios);
   print_info("forward/sieve/promote threads = " + std::to_string(FLAGS_forward_epoch_thread) + "/" +
              std::to_string(FLAGS_sieve_eviction_thread) + "/" + std::to_string(FLAGS_record_cache_promote_thread));

   if (!FLAGS_cxl_tiering_enabled) {
      print_fail("This test requires --cxl_tiering_enabled=true");
      return 1;
   }

   const u64 records_per_page = FLAGS_test_records_per_page;
   const u64 total_pages = static_cast<u64>(FLAGS_test_data_gib * 1024 * 1024 * 1024 / storage::PAGE_SIZE);
   const u64 tuple_count = FLAGS_test_tuple_count ? FLAGS_test_tuple_count : total_pages * records_per_page;
   const u64 num_pages = tuple_count / records_per_page;
   print_info("Computed: tuple_count=" + std::to_string(tuple_count) + ", est_pages=" + std::to_string(num_pages));

   TwoLevelZipfianGenerator zipf_gen;
   zipf_gen.init(num_pages, records_per_page, FLAGS_test_theta_page, FLAGS_test_theta_slot);

   LeanStore db;
   auto& crm = db.getCRManager();
   LeanStoreAdapter<KVTable> table;
   crm.scheduleJobSync(0, [&]() { table = LeanStoreAdapter<KVTable>(db, "CXL_LOOKUP_UPDATE_TEST"); });
   print_pass(std::string("LeanStore initialized, ") + (FLAGS_vi ? "BTreeVI" : "BTreeLL") + " registered");

   try {
      phase1_load_data(crm, table, tuple_count);
      phase2_cxl_verification_and_bandwidth();
      auto ratios = parse_lookup_ratios(FLAGS_test_lookup_update_ratios);
      for (u64 lookup_ratio : ratios) {
         run_mixed_ratio(crm, table, zipf_gen, FLAGS_test_total_ops, lookup_ratio);
      }
      phase5_partition_dump();
   } catch (const std::exception& e) {
      print_fail(std::string("Exception: ") + e.what());
      return 1;
   }

   std::cout << "\n" << Color::BOLD << Color::GREEN
             << "============================================\n"
             << "  CXL Lookup+Update Integration Test COMPLETE\n"
             << "============================================" << Color::RESET << "\n";
   return 0;
}
