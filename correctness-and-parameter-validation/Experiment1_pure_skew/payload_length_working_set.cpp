// ==============================================================================
// Theoretical RecordCache Hit Ratio: 2D Sweep
//   X-axis: payload size        (8B → 500B, step=8B)
//   Y-axis: working set size    (4G / 8G / 16G / 32G / 64G)
//   Fixed:  RecordCache = 1 GiB, zipf_theta = 0.99
//
// Size definitions:
//   record_cache_entry_size = payload + 16B  (key + metadata header)
//   record_on_page_size     = payload + 10B  (key + page-level overhead)
//
// Formula:
//   N        = total_pages * floor(page_size / on_page_size)
//   K        = floor(record_cache / entry_size)
//   coverage = K / N
//   hit_ratio = Σ(1/i^θ, i=1..K) / Σ(1/i^θ, i=1..N)
//
// Usage:
//   ./payload_working_set_payload_variable \
//       --record_cache_gib=1   \
//       --page_size_bytes=16384\
//       --zipf_theta=0.99      \
//       --payload_min=8        \
//       --payload_max=500      \
//       --payload_step=8
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
    double record_cache_gib = 1.0;
    u64    page_size_bytes  = 16384;
    double zipf_theta       = 0.99;
    u64    payload_min      = 8;
    u64    payload_max      = 500;
    u64    payload_step     = 8;

    // Fixed working set sizes
    std::vector<double> working_set_gibs = {4.0, 8.0, 16.0, 32.0, 64.0};

    // Derived from payload
    u64 entry_size(u64 payload)      const { return payload + 16; }
    u64 on_page_size(u64 payload)    const { return payload + 10; }
    u64 records_per_page(u64 payload)const {
        return page_size_bytes / on_page_size(payload);
    }
    u64 cache_capacity(u64 payload)  const {
        return static_cast<u64>(record_cache_gib * GiB) / entry_size(payload);
    }
    u64 total_pages(double ws_gib)   const {
        return static_cast<u64>(ws_gib * GiB) / page_size_bytes;
    }
    u64 total_records(double ws_gib, u64 payload) const {
        return total_pages(ws_gib) * records_per_page(payload);
    }

    void print() const {
        std::cout << "=== 2D Sweep Configuration ===\n";
        std::cout << "  record_cache_gib : " << record_cache_gib << " GiB  (fixed)\n";
        std::cout << "  page_size_bytes  : " << page_size_bytes  << " B\n";
        std::cout << "  zipf_theta       : " << zipf_theta       << "  (fixed)\n";
        std::cout << "  entry_size       = payload + 16B\n";
        std::cout << "  on_page_size     = payload + 10B\n";
        std::cout << "  payload sweep    : [" << payload_min << "B, "
                  << payload_max << "B] step=" << payload_step << "B\n";
        std::cout << "  working_set sweep: ";
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

        if      (key == "record_cache_gib") cfg.record_cache_gib = std::stod(val);
        else if (key == "page_size_bytes")  cfg.page_size_bytes  = std::stoull(val);
        else if (key == "zipf_theta")       cfg.zipf_theta       = std::stod(val);
        else if (key == "payload_min")      cfg.payload_min      = std::stoull(val);
        else if (key == "payload_max")      cfg.payload_max      = std::stoull(val);
        else if (key == "payload_step")     cfg.payload_step     = std::stoull(val);
        else throw std::invalid_argument("unknown arg: --" + key);
    }
    return cfg;
}

// ============================================================
// Theoretical hit ratio
//   Exact sum up to exact_limit, integral tail beyond
// ============================================================
double computeHitRatio(u64 N, u64 K, double theta) {
    if (N == 0 || K == 0) return 0.0;
    K = std::min(K, N);

    const u64    exact_limit = std::min(N, u64{5'000'000});
    const double exp_val     = 1.0 - theta;

    double zeta_n = 0.0, zeta_k = 0.0;

    for (u64 i = 1; i <= exact_limit; i++) {
        const double t = std::pow(1.0 / static_cast<double>(i), theta);
        zeta_n += t;
        if (i <= K) zeta_k += t;
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

    return (zeta_n > 0.0) ? zeta_k / zeta_n : 0.0;
}

// ============================================================
// Data cell
// ============================================================
struct Cell {
    u64    payload;
    double working_set_gib;
    u64    records_per_page;
    u64    total_records;    // N
    u64    cache_capacity;   // K
    double coverage_pct;
    double hit_ratio_pct;
};

// ============================================================
// Main
// ============================================================
int main(int argc, char** argv) {
    try {
        const Config cfg = parseArgs(argc, argv);
        cfg.print();

        if (cfg.payload_min == 0 || cfg.payload_step == 0)
            throw std::invalid_argument("payload_min/step must be > 0");

        // Collect all payload values
        std::vector<u64> payloads;
        for (u64 p = cfg.payload_min; p <= cfg.payload_max; p += cfg.payload_step)
            payloads.push_back(p);

        // Build 2D result matrix [working_set][payload]
        // rows = working_set, cols = payload
        const size_t nWS  = cfg.working_set_gibs.size();
        const size_t nPay = payloads.size();
        std::vector<std::vector<Cell>> matrix(nWS, std::vector<Cell>(nPay));

        for (size_t wi = 0; wi < nWS; wi++) {
            const double ws = cfg.working_set_gibs[wi];
            for (size_t pi = 0; pi < nPay; pi++) {
                const u64 p   = payloads[pi];
                const u64 rpp = cfg.records_per_page(p);
                const u64 N   = cfg.total_records(ws, p);
                const u64 K   = cfg.cache_capacity(p);

                Cell& c          = matrix[wi][pi];
                c.payload        = p;
                c.working_set_gib= ws;
                c.records_per_page = rpp;
                c.total_records  = N;
                c.cache_capacity = K;
                c.coverage_pct   = (N > 0) ? (double)K / N * 100.0 : 0.0;
                c.hit_ratio_pct  = (N >= 2 && rpp > 0)
                                   ? computeHitRatio(N, K, cfg.zipf_theta) * 100.0
                                   : 0.0;
            }
        }

        // --------------------------------------------------------
        // Print 2D table: rows=working_set, cols=payload
        // Cell value = hit_ratio_pct
        // --------------------------------------------------------
        std::cout << "=== Hit Ratio (%) - rows: working_set, cols: payload ===\n\n";

        // Header
        std::cout << std::setw(14) << "WS \\ payload";
        for (u64 p : payloads)
            std::cout << std::setw(8) << p;
        std::cout << "\n";
        std::cout << std::string(14 + 8 * nPay, '-') << "\n";

        // Rows
        std::cout << std::fixed << std::setprecision(1);
        for (size_t wi = 0; wi < nWS; wi++) {
            std::cout << std::setw(12) << cfg.working_set_gibs[wi] << "G ";
            for (size_t pi = 0; pi < nPay; pi++)
                std::cout << std::setw(8) << matrix[wi][pi].hit_ratio_pct;
            std::cout << "\n";
        }
        std::cout << "\n";

        // --------------------------------------------------------
        // Print 2D table: coverage_pct
        // --------------------------------------------------------
        std::cout << "=== Coverage (%) - rows: working_set, cols: payload ===\n\n";
        std::cout << std::setw(14) << "WS \\ payload";
        for (u64 p : payloads)
            std::cout << std::setw(8) << p;
        std::cout << "\n";
        std::cout << std::string(14 + 8 * nPay, '-') << "\n";

        std::cout << std::fixed << std::setprecision(1);
        for (size_t wi = 0; wi < nWS; wi++) {
            std::cout << std::setw(12) << cfg.working_set_gibs[wi] << "G ";
            for (size_t pi = 0; pi < nPay; pi++)
                std::cout << std::setw(8) << matrix[wi][pi].coverage_pct;
            std::cout << "\n";
        }
        std::cout << "\n";

        // --------------------------------------------------------
        // CSV: one row per (working_set, payload) combination
        // --------------------------------------------------------
        std::cout << "=== CSV (for plotting) ===\n";
        std::cout << "working_set_gib,payload_bytes,entry_size,on_page_size,"
                  << "records_per_page,total_records,cache_capacity,"
                  << "coverage_pct,hit_ratio_pct\n";
        std::cout << std::fixed << std::setprecision(4);
        for (size_t wi = 0; wi < nWS; wi++) {
            for (size_t pi = 0; pi < nPay; pi++) {
                const Cell& c = matrix[wi][pi];
                std::cout << cfg.working_set_gibs[wi]   << ","
                          << c.payload                   << ","
                          << cfg.entry_size(c.payload)   << ","
                          << cfg.on_page_size(c.payload) << ","
                          << c.records_per_page          << ","
                          << c.total_records             << ","
                          << c.cache_capacity            << ","
                          << c.coverage_pct              << ","
                          << c.hit_ratio_pct             << "\n";
            }
        }

        // --------------------------------------------------------
        // Summary: min/max hit_ratio per working_set
        // --------------------------------------------------------
        std::cout << "\n=== Summary: hit_ratio range per working_set ===\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << std::setw(14) << "working_set"
                  << std::setw(16) << "min_hit_ratio%"
                  << std::setw(10) << "payload"
                  << std::setw(16) << "max_hit_ratio%"
                  << std::setw(10) << "payload"
                  << std::setw(12) << "delta(pp)"
                  << "\n";
        std::cout << std::string(78, '-') << "\n";

        for (size_t wi = 0; wi < nWS; wi++) {
            double min_hr = 1e9, max_hr = -1e9;
            u64    min_p  = 0,   max_p  = 0;
            for (size_t pi = 0; pi < nPay; pi++) {
                const Cell& c = matrix[wi][pi];
                if (c.hit_ratio_pct < min_hr) { min_hr = c.hit_ratio_pct; min_p = c.payload; }
                if (c.hit_ratio_pct > max_hr) { max_hr = c.hit_ratio_pct; max_p = c.payload; }
            }
            std::cout << std::setw(12) << cfg.working_set_gibs[wi] << "G"
                      << std::setw(16) << min_hr
                      << std::setw(10) << min_p
                      << std::setw(16) << max_hr
                      << std::setw(10) << max_p
                      << std::setw(12) << (max_hr - min_hr)
                      << "\n";
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nError: " << ex.what() << "\n\n";
        std::cerr << "Usage:\n";
        std::cerr << "  ./payload_length_working_set \\\n";
        std::cerr << "      --record_cache_gib=1    \\\n";
        std::cerr << "      --page_size_bytes=16384 \\\n";
        std::cerr << "      --zipf_theta=0.99       \\\n";
        std::cerr << "      --payload_min=8         \\\n";
        std::cerr << "      --payload_max=500       \\\n";
        std::cerr << "      --payload_step=8\n";
        return 1;
    }
}
