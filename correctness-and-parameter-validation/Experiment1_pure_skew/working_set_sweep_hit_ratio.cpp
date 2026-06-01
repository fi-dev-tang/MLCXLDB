// ==============================================================================
// Theoretical RecordCache Hit Ratio vs. Working Set Size
//
// Fixes RecordCache = 1 GiB, zipf_theta = 0.99
// Sweeps working_set_gib: 4 -> 8 -> 16 -> 32 -> 64 GiB
//
// Formula:
//   N = total_records = total_pages * records_per_page
//   K = cache_capacity = record_cache_gib / record_cache_entry_size
//   coverage = K / N
//   hit_ratio = Σ(1/i^θ, i=1..K) / Σ(1/i^θ, i=1..N)
//
// Usage:
//   ./working_set_sweep_hit_ratio \
//       --record_cache_gib=1 \
//       --record_cache_entry_size=24 \
//       --record_size_bytes=16 \
//       --page_size_bytes=16384 \
//       --zipf_theta=0.99
// ==============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using u64 = std::uint64_t;
constexpr u64 GiB = 1024ULL * 1024ULL * 1024ULL;

// ============================================================
// Config
// ============================================================
struct Config {
    double record_cache_gib        = 1.0;
    u64    record_cache_entry_size = 24;    // payload(16) + header(8)
    u64    record_size_bytes       = 16;    // payload on page
    u64    page_size_bytes         = 16384;
    double zipf_theta              = 0.99;

    // Fixed working set sizes to sweep
    std::vector<double> working_set_gibs = {4.0, 8.0, 16.0, 32.0, 64.0};

    u64 records_per_page() const {
        return page_size_bytes / record_size_bytes;
    }
    u64 cache_capacity() const {
        return static_cast<u64>(record_cache_gib * GiB) / record_cache_entry_size;
    }

    void print() const {
        std::cout << "=== Configuration ===\n";
        std::cout << "  record_cache_gib        : " << record_cache_gib        << " GiB\n";
        std::cout << "  record_cache_entry_size : " << record_cache_entry_size << " B"
                  << "  (payload+" << record_cache_entry_size - record_size_bytes << "B header)\n";
        std::cout << "  record_size_bytes       : " << record_size_bytes       << " B\n";
        std::cout << "  page_size_bytes         : " << page_size_bytes         << " B\n";
        std::cout << "  records_per_page        : " << records_per_page()      << "\n";
        std::cout << "  cache_capacity (K)      : " << cache_capacity()        << " records\n";
        std::cout << "  zipf_theta              : " << zipf_theta              << "\n";
        std::cout << "  working_set sweep       : ";
        for (double g : working_set_gibs) std::cout << g << "G ";
        std::cout << "\n\n";
    }
};

// ============================================================
// Argument parsing
// ============================================================
Config parseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        const auto eq = arg.find('=');
        if (eq == std::string::npos || arg.rfind("--", 0) != 0)
            throw std::invalid_argument("invalid arg: " + arg);
        const std::string key = arg.substr(2, eq - 2);
        const std::string val = arg.substr(eq + 1);

        if      (key == "record_cache_gib")        cfg.record_cache_gib        = std::stod(val);
        else if (key == "record_cache_entry_size") cfg.record_cache_entry_size = std::stoull(val);
        else if (key == "record_size_bytes")       cfg.record_size_bytes       = std::stoull(val);
        else if (key == "page_size_bytes")         cfg.page_size_bytes         = std::stoull(val);
        else if (key == "zipf_theta")              cfg.zipf_theta              = std::stod(val);
        else throw std::invalid_argument("unknown arg: --" + key);
    }
    return cfg;
}

// ============================================================
// Theoretical hit ratio
//   hit_ratio = Σ(1/i^θ, i=1..K) / Σ(1/i^θ, i=1..N)
//   Exact up to exact_limit, integral tail beyond
// ============================================================
double computeTheoreticalHitRatio(u64 N, u64 K, double theta) {
    if (N == 0 || K == 0) return 0.0;
    K = std::min(K, N);

    const u64    exact_limit = std::min(N, u64{5'000'000});
    const double exp_val     = 1.0 - theta;

    double zeta_n = 0.0;
    double zeta_k = 0.0;

    for (u64 i = 1; i <= exact_limit; i++) {
        const double term = std::pow(1.0 / static_cast<double>(i), theta);
        zeta_n += term;
        if (i <= K) zeta_k += term;
    }

    if (N > exact_limit) {
        const double L  = static_cast<double>(exact_limit) + 0.5;
        const double Nd = static_cast<double>(N)           + 0.5;
        zeta_n += (std::pow(Nd, exp_val) - std::pow(L, exp_val)) / exp_val;

        if (K > exact_limit) {
            const double Kd = static_cast<double>(K) + 0.5;
            zeta_k += (std::pow(Kd, exp_val) - std::pow(L, exp_val)) / exp_val;
        }
    }

    return (zeta_n > 0.0) ? (zeta_k / zeta_n) : 0.0;
}

// ============================================================
// Result row
// ============================================================
struct Row {
    double working_set_gib;
    u64    total_pages;
    u64    total_records;    // N
    u64    cache_capacity;   // K
    double coverage_pct;     // K/N %
    double hit_ratio_pct;
};

// ============================================================
// Main
// ============================================================
int main(int argc, char** argv) {
    try {
        const Config cfg = parseArgs(argc, argv);
        cfg.print();

        const u64 rpp = cfg.records_per_page();
        const u64 K   = cfg.cache_capacity();

        if (rpp == 0)
            throw std::invalid_argument(
                "records_per_page = 0: page_size too small or record_size too large");

        std::vector<Row> rows;

        for (double ws_gib : cfg.working_set_gibs) {
            const u64 total_pages   = static_cast<u64>(ws_gib * GiB) / cfg.page_size_bytes;
            const u64 total_records = total_pages * rpp;  // N

            if (total_records < 2) {
                std::cerr << "  [SKIP] working_set=" << ws_gib
                          << "G: total_records < 2\n";
                continue;
            }

            const double coverage   = (double)K / total_records * 100.0;
            const double hit_ratio  = computeTheoreticalHitRatio(
                                          total_records, K, cfg.zipf_theta);

            rows.push_back({ws_gib, total_pages, total_records,
                            K, coverage, hit_ratio * 100.0});
        }

        // --------------------------------------------------------
        // Print table
        // --------------------------------------------------------
        const int w1 = 16;   // working_set
        const int w2 = 14;   // total_pages
        const int w3 = 16;   // total_records
        const int w4 = 16;   // cache_capacity
        const int w5 = 13;   // coverage%
        const int w6 = 13;   // hit_ratio%

        const int total_w = w1+w2+w3+w4+w5+w6+2;
        std::cout << std::string(total_w, '=') << "\n";
        std::cout << std::setw(w1) << "working_set(G)"
                  << std::setw(w2) << "total_pages"
                  << std::setw(w3) << "total_rec(M)"
                  << std::setw(w4) << "cache_cap(M)"
                  << std::setw(w5) << "coverage%"
                  << std::setw(w6) << "hit_ratio%"
                  << "\n";
        std::cout << std::string(total_w, '-') << "\n";

        std::cout << std::fixed;
        for (const auto& r : rows) {
            std::cout << std::setw(w1) << std::setprecision(0) << r.working_set_gib
                      << std::setw(w2) << r.total_pages
                      << std::setw(w3) << std::setprecision(2) << r.total_records / 1e6
                      << std::setw(w4) << std::setprecision(2) << r.cache_capacity / 1e6
                      << std::setw(w5) << std::setprecision(2) << r.coverage_pct
                      << std::setw(w6) << std::setprecision(2) << r.hit_ratio_pct
                      << "\n";
        }
        std::cout << std::string(total_w, '=') << "\n";

        // --------------------------------------------------------
        // CSV
        // --------------------------------------------------------
        std::cout << "\n=== CSV (for plotting) ===\n";
        std::cout << "working_set_gib,total_pages,total_records,"
                  << "cache_capacity,coverage_pct,hit_ratio_pct\n";
        for (const auto& r : rows) {
            std::cout << std::fixed << std::setprecision(1) << r.working_set_gib  << ","
                      << r.total_pages    << ","
                      << r.total_records  << ","
                      << r.cache_capacity << ","
                      << std::setprecision(4) << r.coverage_pct  << ","
                      << std::setprecision(4) << r.hit_ratio_pct << "\n";
        }

        // --------------------------------------------------------
        // Interpretation
        // --------------------------------------------------------
        std::cout << "\n=== Interpretation ===\n";
        std::cout << "  Fixed: RecordCache=" << cfg.record_cache_gib
                  << "GiB, K=" << K << " records, θ=" << cfg.zipf_theta << "\n\n";
        for (const auto& r : rows) {
            std::cout << "  working_set=" << std::setw(4) << std::setprecision(0)
                      << r.working_set_gib << "G"
                      << "  coverage=" << std::setw(6) << std::setprecision(2)
                      << r.coverage_pct << "%"
                      << "  hit_ratio_upper_bound=" << std::setw(6)
                      << std::setprecision(2) << r.hit_ratio_pct << "%"
                      << "  impl_gap_from_20%=" << std::setw(6)
                      << std::setprecision(2) << r.hit_ratio_pct - 20.0 << "pp"
                      << "\n";
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nError: " << ex.what() << "\n\n";
        std::cerr << "Usage:\n";
        std::cerr << "  ./working_set_sweep_hit_ratio \\\n";
        std::cerr << "      --record_cache_gib=1        \\\n";
        std::cerr << "      --record_cache_entry_size=24 \\\n";
        std::cerr << "      --record_size_bytes=16      \\\n";
        std::cerr << "      --page_size_bytes=16384     \\\n";
        std::cerr << "      --zipf_theta=0.99\n";
        return 1;
    }
}
