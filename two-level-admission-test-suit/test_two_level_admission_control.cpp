#include <iostream>
#include <vector>
#include <cassert>
#include <iomanip>
#include <fstream>
#include <string>
#include <chrono>

#include "backend/leanstore/storage/two-level-admission-control/TwoLevelAdmissionControl.hpp"
#include "backend/leanstore/Config.hpp"
#include <random>
#include <cmath>

// Manually define the gflags variables to avoid linking against gflags library
namespace fLD {
    double FLAGS_dram_buffer_pool_gib = 16.0;
    double FLAGS_cxl_gib = 32.0;
}
namespace fLB {
    bool FLAGS_cxl_tiering_enabled = true;
}

using namespace leanstore::storage::two_level_admission_control;
using namespace leanstore;

class ZipfGenerator {
private:
    uint64_t n_;
    double theta_;
    double alpha_;
    double zeta_n_;
    double eta_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> dist_;

    double zeta_approx(uint64_t n, double theta) {
        double sum = 0.0;
        uint64_t exact_limit = std::min(n, (uint64_t)1000);
        for (uint64_t i = 1; i <= exact_limit; i++) {
            sum += 1.0 / std::pow((double)i, theta);
        }
        if (n > exact_limit) {
            if (std::abs(theta - 1.0) < 1e-9) {
                sum += std::log((double)n / (double)exact_limit);
            } else {
                sum += (std::pow((double)n, 1.0 - theta) 
                        - std::pow((double)exact_limit, 1.0 - theta))
                       / (1.0 - theta);
            }
        }
        return sum;
    }

    double zeta_exact(uint64_t n, double theta) {
        double sum = 0.0;
        for (uint64_t i = 1; i <= n; i++)
            sum += 1.0 / std::pow((double)i, theta);
        return sum;
    }

public:
    ZipfGenerator(uint64_t n, double theta, uint64_t seed = 42)
        : n_(n), theta_(theta), dist_(0.0, 1.0) {
        rng_.seed(seed);
        zeta_n_ = zeta_approx(n_, theta_);
        double zeta_2 = zeta_exact(2, theta_);
        alpha_ = 1.0 / (1.0 - theta_);
        eta_ = (1.0 - std::pow(2.0 / (double)n_, 1.0 - theta_)) /
               (1.0 - zeta_2 / zeta_n_);
    }

    // 返回 [0, n-1]，0 最热
    uint64_t next() {
        double u = dist_(rng_);
        double uz = u * zeta_n_;
        if (uz < 1.0) return 0;
        if (uz < 1.0 + std::pow(0.5, theta_)) return 1;
        uint64_t v = (uint64_t)((double)n_ * std::pow(eta_ * u - eta_ + 1.0, alpha_));
        if (v >= n_) v = n_ - 1;
        return v;
    }
};

uint64_t fnv1a(uint64_t val) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 8; i++) {
        hash ^= (val & 0xFF);
        hash *= 0x100000001b3ULL;
        val >>= 8;
    }
    return hash;
}

struct RecordLocation {
    uint64_t page_id;
    uint16_t slot_id;
};

RecordLocation record_to_location(uint64_t record_id,
                                   uint64_t total_pages,
                                   uint16_t slots_per_page) {
    uint64_t h1 = fnv1a(record_id);
    uint64_t h2 = fnv1a(record_id ^ 0xdeadbeefcafeULL);
    return {h1 % total_pages, (uint16_t)(h2 % slots_per_page)};
}

void RunSimulation(double dram_gib, double cxl_gib, double zipf_theta, int total_rounds) {
    // 1. Set global flags for DRAM/CXL ratio
    FLAGS_dram_buffer_pool_gib = dram_gib;
    FLAGS_cxl_gib = cxl_gib;
    FLAGS_cxl_tiering_enabled = true;

    std::string ratio_str = "1:" + std::to_string(static_cast<int>(cxl_gib / dram_gib));
    std::string filename = "simulation_result_ratio_" + ratio_str + ".txt";
    std::ofstream out_file(filename);

    out_file << "=================================================================\n";
    out_file << "Simulation Started\n";
    out_file << "DRAM:CXL Ratio = " << ratio_str << " (" << dram_gib << " GiB : " << cxl_gib << " GiB)\n";
    out_file << "Zipfian Theta = " << zipf_theta << "\n";
    out_file << "Total Rounds (10k requests/round) = " << total_rounds << "\n";
    out_file << "=================================================================\n\n";

    std::cout << "Running simulation for ratio " << ratio_str << " (Output to " << filename << ")...\n";

    // 2. Initialize core components
    PageCountMinSketch page_cms(12, 1024 * 16);
    RecordCountMinSketch record_cms(12, 1024 * 8);
    
    // 13 buckets as requested
    VisitFrequencyBucketArray bucket_array(13);
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    SampledVisitHistogram histogram(page_cms, sampling_set, bucket_array);
    
    DramHotPageCandidates hot_page_candidates(record_cms);

    // 3. Initialize Facade
    TwoLevelAdmissionControl admission_control(
        page_cms,
        histogram,
        record_cms,
        hot_page_candidates
    );

    // 4. Setup Zipfian Generator for Records
    u16 slots_per_page = 175; // Default slots per page
    u64 total_pages = 262144; // e.g., 4GB dataset = 262144 pages
    u64 total_records = total_pages * slots_per_page; // ~45.8M records
    
    // Generate logical record IDs with strict Zipfian skew (0 is hottest)
    ZipfGenerator zipf_gen(total_records, zipf_theta, 12345);

    // 5. Simulation Loop
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int round = 1; round <= total_rounds; ++round) {
        // Each round is 10,000 requests to trigger histogram update
        for (int i = 0; i < 10000; ++i) {
            // Generate a logical record ID
            u64 logical_record_id = zipf_gen.next();
            
            // Hash the logical record ID to scatter it randomly across all pages and slots.
            RecordLocation loc = record_to_location(logical_record_id, total_pages, slots_per_page);

            // Worker Thread action
            admission_control.OnRecordAccess(loc.page_id, loc.slot_id, false, slots_per_page);
        }

        // Background Thread action (triggers every 10k requests automatically)
        auto decisions = admission_control.BackgroundRoutine();

        // Print histogram and decisions every round
        out_file << "\n--- Round " << round << " (Total Requests: " << round * 10000 << ") ---\n";
        out_file << histogram.PrintHistogram();
        
        out_file << "\nPromotion Decisions (Total: " << decisions.size() << "):\n";
        u64 full_page_promotions = 0;
        u64 record_promotions = 0;
        
        for (const auto& decision : decisions) {
            if (decision.promote_entire_page) {
                full_page_promotions++;
            } else {
                record_promotions += decision.hot_slot_ids.size();
            }
        }
        out_file << "- Full Page Promotions: " << full_page_promotions << "\n";
        out_file << "- Skewed Record Promotions: " << record_promotions << "\n";
        out_file << "------------------------------------------------------\n";
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    out_file << "\nSimulation Completed in " << elapsed.count() << " seconds.\n";
    out_file.close();
    
    std::cout << "✓ Finished ratio " << ratio_str << " in " << elapsed.count() << "s\n";
}

int main() {
    std::cout << "Starting Two-Level Admission Control Tests...\n\n";

    double zipf_theta = 0.99;
    int total_rounds = 3000; // 3000 rounds * 10,000 requests = 30,000,000 requests per simulation

    // Test 1: DRAM:CXL = 1:2
    RunSimulation(16.0, 32.0, zipf_theta, total_rounds);

    // Test 2: DRAM:CXL = 1:3
    RunSimulation(16.0, 48.0, zipf_theta, total_rounds);

    // Test 3: DRAM:CXL = 1:4
    RunSimulation(16.0, 64.0, zipf_theta, total_rounds);

    // Test 4: DRAM:CXL = 1:8
    RunSimulation(16.0, 128.0, zipf_theta, total_rounds);

    std::cout << "\nAll simulations completed successfully. Check the generated .txt files.\n";
    return 0;
}
