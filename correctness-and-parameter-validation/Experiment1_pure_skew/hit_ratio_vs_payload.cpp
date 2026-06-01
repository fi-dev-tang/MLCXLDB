// ==============================================================================
// Theoretical RecordCache Hit Ratio vs. Record Payload Size
//
// Sweeps record payload size from 8B to 200B and computes the theoretical
// upper bound hit ratio for each configuration.
//
// record_cache_entry_size = payload + 16B  (key + metadata header)
// record_on_page_size     = payload + 10B  (key + page-level overhead)
//
// Formula:
//   N = total_records = working_set / record_on_page_size
//   K = cache_capacity = record_cache_gib / record_cache_entry_size
//   hit_ratio = Σ(1/i^θ, i=1..K) / Σ(1/i^θ, i=1..N)
//
// Usage:
//   ./hit_ratio_vs_payload \
//       --working_set_gib=4 \
//       --record_cache_gib=1 \
//       --zipf_theta=0.99 \
//       --payload_min=8 \
//       --payload_max=200 \
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
    double working_set_gib  = 4.0;
    double record_cache_gib = 1.0;
    double zipf_theta       = 0.99;
    u64    payload_min      = 8;
    u64    payload_max      = 200;
    u64    payload_step     = 8;
    u64    page_size_bytes  = 16384;

    void print() const {
        std::cout << "=== Sweep Configuration ===\n";
        std::cout << "  working_set_gib  : " << working_set_gib  << " GiB\n";
        std::cout << "  record_cache_gib : " << record_cache_gib << " GiB\n";
        std::cout << "  zipf_theta       : " << zipf_theta       << "\n";
        std::cout << "  page_size_bytes  : " << page_size_bytes  << " B\n";
        std::cout << "  payload range    : [" << payload_min
                  << "B, " << payload_max << "B] step=" << payload_step << "B\n";
        std::cout << "  entry_size       = payload + 16B  (RecordCache)\n";
        std::cout << "  on_page_size     = payload + 10B  (Page slot)\n\n";
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

        if      (key == "working_set_gib")  cfg.working_set_gib  = std::stod(val);
        else if (key == "record_cache_gib") cfg.record_cache_gib = std::stod(val);
        else if (key == "zipf_theta")       cfg.zipf_theta       = std::stod(val);
        else if (key == "payload_min")      cfg.payload_min      = std::stoull(val);
        else if (key == "payload_max")      cfg.payload_max      = std::stoull(val);
        else if (key == "payload_step")     cfg.payload_step     = std::stoull(val);
        else if (key == "page_size_bytes")  cfg.page_size_bytes  = std::stoull(val);
        else throw std::invalid_argument("unknown arg: --" + key);
    }
    return cfg;
}

// ============================================================
// Per-payload derived quantities
// ============================================================
struct PayloadConfig {
    u64    payload_bytes;
    u64    entry_size;          // payload + 16B  (RecordCache entry)
    u64    on_page_size;        // payload + 10B  (slot on page)
    u64    records_per_page;    // floor(page_size / on_page_size)
    u64    total_pages;         // working_set / page_size
    u64    total_records;       // total_pages * records_per_page
    u64    cache_capacity;      // record_cache / entry_size
    double coverage_pct;        // cache_capacity / total_records * 100
};

PayloadConfig buildPayloadConfig(u64 payload, const Config& cfg) {
    PayloadConfig pc;
    pc.payload_bytes    = payload;
    pc.entry_size       = payload + 16;
    pc.on_page_size     = payload + 10;
    pc.records_per_page = cfg.page_size_bytes / pc.on_page_size;
    pc.total_pages      = static_cast<u64>(cfg.working_set_gib * GiB)
                          / cfg.page_size_bytes;
    pc.total_records    = pc.total_pages * pc.records_per_page;
    pc.cache_capacity   = static_cast<u64>(cfg.record_cache_gib * GiB)
                          / pc.entry_size;
    pc.coverage_pct     = (pc.total_records == 0) ? 0.0 :
                          (double)pc.cache_capacity / pc.total_records * 100.0;
    return pc;
}

// ============================================================
// Theoretical hit ratio
//   hit_ratio = Σ(1/i^θ, i=1..K) / Σ(1/i^θ, i=1..N)
//
// For large N: exact up to exact_limit, then integral tail approximation
//   ∫_L^{N+0.5} x^{-θ} dx = [x^{1-θ}/(1-θ)]_L^{N+0.5}
// ============================================================
double computeTheoreticalHitRatio(u64 N, u64 K, double theta) {
    if (N == 0 || K == 0) return 0.0;
    K = std::min(K, N);

    const u64    exact_limit = std::min(N, u64{5'000'000});
    const double exp_val     = 1.0 - theta;   // > 0 since theta < 1

    double zeta_n = 0.0;
    double zeta_k = 0.0;

    for (u64 i = 1; i <= exact_limit; i++) {
        const double term = std::pow(1.0 / static_cast<double>(i), theta);
        zeta_n += term;
        if (i <= K) zeta_k += term;
    }

    // Tail for zeta_n: integral from exact_limit+0.5 to N+0.5
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
    u64    payload;
    u64    entry_size;
    u64    on_page_size;
    u64    records_per_page;
    u64    total_records;
    u64    cache_capacity;
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
            throw std::invalid_argument("payload_min and payload_step must be > 0");
        if (cfg.payload_min > cfg.payload_max)
            throw std::invalid_argument("payload_min must be <= payload_max");

        std::vector<Row> rows;

        // Sweep payload size
        for (u64 payload = cfg.payload_min;
             payload <= cfg.payload_max;
             payload += cfg.payload_step)
        {
            const PayloadConfig pc = buildPayloadConfig(payload, cfg);

            if (pc.records_per_page == 0) {
                std::cerr << "  [SKIP] payload=" << payload
                          << "B: on_page_size=" << pc.on_page_size
                          << " > page_size=" << cfg.page_size_bytes << "\n";
                continue;
            }
            if (pc.total_records < 2) {
                std::cerr << "  [SKIP] payload=" << payload
                          << "B: total_records < 2\n";
                continue;
            }

            const double hit_ratio = computeTheoreticalHitRatio(
                pc.total_records, pc.cache_capacity, cfg.zipf_theta);

            rows.push_back({
                payload,
                pc.entry_size,
                pc.on_page_size,
                pc.records_per_page,
                pc.total_records,
                pc.cache_capacity,
                pc.coverage_pct,
                hit_ratio * 100.0
            });
        }

        // --------------------------------------------------------
        // Print table
        // --------------------------------------------------------
        const int w1  = 10;   // payload
        const int w2  = 12;   // entry_size
        const int w3  = 13;   // on_page_size
        const int w4  = 16;   // records_per_page
        const int w5  = 16;   // total_records
        const int w6  = 16;   // cache_capacity
        const int w7  = 13;   // coverage%
        const int w8  = 14;   // hit_ratio%

        std::cout << std::string(w1+w2+w3+w4+w5+w6+w7+w8+2, '=') << "\n";
        std::cout << std::setw(w1) << "payload(B)"
                  << std::setw(w2) << "entry(B)"
                  << std::setw(w3) << "on_page(B)"
                  << std::setw(w4) << "rec/page"
                  << std::setw(w5) << "total_rec(M)"
                  << std::setw(w6) << "cache_cap(M)"
                  << std::setw(w7) << "coverage%"
                  << std::setw(w8) << "hit_ratio%"
                  << "\n";
        std::cout << std::string(w1+w2+w3+w4+w5+w6+w7+w8+2, '-') << "\n";

        std::cout << std::fixed;
        for (const auto& r : rows) {
            std::cout << std::setw(w1) << r.payload
                      << std::setw(w2) << r.entry_size
                      << std::setw(w3) << r.on_page_size
                      << std::setw(w4) << r.records_per_page
                      << std::setw(w5) << std::setprecision(2)
                                       << r.total_records / 1e6
                      << std::setw(w6) << std::setprecision(2)
                                       << r.cache_capacity / 1e6
                      << std::setw(w7) << std::setprecision(2)
                                       << r.coverage_pct
                      << std::setw(w8) << std::setprecision(2)
                                       << r.hit_ratio_pct
                      << "\n";
        }
        std::cout << std::string(w1+w2+w3+w4+w5+w6+w7+w8+2, '=') << "\n";

        // --------------------------------------------------------
        // CSV output for plotting
        // --------------------------------------------------------
        std::cout << "\n=== CSV (for plotting) ===\n";
        std::cout << "payload_bytes,entry_size,on_page_size,records_per_page,"
                  << "total_records,cache_capacity,coverage_pct,hit_ratio_pct\n";
        for (const auto& r : rows) {
            std::cout << r.payload      << ","
                      << r.entry_size   << ","
                      << r.on_page_size << ","
                      << r.records_per_page << ","
                      << r.total_records << ","
                      << r.cache_capacity << ","
                      << std::fixed << std::setprecision(4)
                      << r.coverage_pct  << ","
                      << r.hit_ratio_pct << "\n";
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nError: " << ex.what() << "\n\n";
        std::cerr << "Usage:\n";
        std::cerr << "  ./hit_ratio_vs_payload \\\n";
        std::cerr << "      --working_set_gib=4   \\\n";
        std::cerr << "      --record_cache_gib=1  \\\n";
        std::cerr << "      --zipf_theta=0.99     \\\n";
        std::cerr << "      --payload_min=8       \\\n";
        std::cerr << "      --payload_max=200     \\\n";
        std::cerr << "      --payload_step=8      \\\n";
        std::cerr << "      --page_size_bytes=16384\n";
        return 1;
    }
}
