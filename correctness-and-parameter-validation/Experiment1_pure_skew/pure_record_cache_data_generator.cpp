// ==============================================================================
// This file is used to test Two-Level-Admission-Control for Skewed Records
// If Record follows Zipfian distribution, meanwhile we scramble it to pages
// The page should all look like skew page
//
// Input: RecordCache Size(in GiB), RecordCacheEntrySize, Zipfian parameter, WorkingSet size
// Output: Theoretical hit ratio.
//
// Theoretical upper limit:
// working_set = 4 GB
// record_cahce_entry_size = 100 B
// total_records = 4 GB / 100 B = 42,949,672
// cache_capacity = 1/4
// Assume RecordCache has cached top 25% record, then
// hit_ratio(caching top 25% has corresponding 75% visit).
// 
// Usage:
//
// if payload_size = 8B, record_cache_entry_size = sizeof(RecordCacheEntry) + payload_size = 16 + 8 = 24 B.
// payload_size = 8B, in BTreeNode, record_size_bytes = sizeof(Slot) + payload_size = 10 + 8 = 18 Byte.
// we does not include key_size.
// ./pure_record_cache_data_generator \
//     --record_cache_gib=1 \
//     --record_cache_entry_size=100 \
//     --working_set_gib=4 \
//     --record_size_bytes=100 \
//     --page_size_bytes=16384 \
//     --zipf_theta=0.99 \
//     --sample_count=10000000 \
//     --skew_top1_threshold=0.10
// ==============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using u64 = std::uint64_t;
constexpr u64 GiB = 1024ULL * 1024ULL * 1024ULL;

// ============================================================
// Config
// ============================================================
struct Config {
    double   fill_factor             = 0.5;   // B+Tree fill factor, default 50%
    double   record_cache_gib        = 1.0;   // RecordCache size in GiB
    u64      record_cache_entry_size = 24;   // bytes per cached record entry
    double   zipf_theta              = 0.99;  // Zipfian skew parameter
    double   working_set_gib         = 4.0;   // total working set size in GiB
    u64      record_size_bytes       = 16;   // bytes per record (on disk/page)
    u64      page_size_bytes         = 16384; // 16 KB
    u64      sample_count            = 10'000'000ULL;
    u64      seed                    = 42;
    double   skew_top1_threshold     = 0.10;  // a slot is skew if top1 > 10% of page hits

    u64 records_per_page() const {
        return static_cast<u64>(page_size_bytes / record_size_bytes) * fill_factor;
    }
    u64 total_records() const {
        return static_cast<u64>(working_set_gib * GiB) / record_size_bytes;
    }
    u64 total_pages() const {
        const u64 rpp = records_per_page();
        return (total_records() + rpp - 1) / rpp;
    }
    u64 record_cache_capacity() const {
        // How many records can RecordCache hold?
        return static_cast<u64>(record_cache_gib * GiB) / record_cache_entry_size;
    }

    void print() const {
        std::cout << "\n=== Input Configuration ===\n";
        std::cout << "  working_set_gib          : " << working_set_gib          << " GiB\n";
        std::cout << "  record_size_bytes        : " << record_size_bytes        << " B\n";
        std::cout << "  page_size_bytes          : " << page_size_bytes          << " B\n";
        std::cout << "  records_per_page         : " << records_per_page()       << "\n";
        std::cout << "  total_records            : " << total_records()          << "\n";
        std::cout << "  total_pages              : " << total_pages()            << "\n";
        std::cout << "  zipf_theta               : " << zipf_theta               << "\n";
        std::cout << "  record_cache_gib         : " << record_cache_gib         << " GiB\n";
        std::cout << "  record_cache_entry_size  : " << record_cache_entry_size  << " B\n";
        std::cout << "  record_cache_capacity    : " << record_cache_capacity()  << " records\n";
        std::cout << "  coverage_ratio           : "
                  << std::fixed << std::setprecision(2)
                  << (double)record_cache_capacity() / total_records() * 100.0
                  << "% of total records\n";
        std::cout << "  sample_count             : " << sample_count             << "\n";
        std::cout << "  seed                     : " << seed                     << "\n";
        std::cout << "  skew_top1_threshold      : " << skew_top1_threshold * 100.0 << "%\n";
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
            throw std::invalid_argument("invalid arg: " + arg + " (expected --key=value)");
        const std::string key = arg.substr(2, eq - 2);
        const std::string val = arg.substr(eq + 1);

        if      (key == "record_cache_gib")        cfg.record_cache_gib        = std::stod(val);
        else if (key == "record_cache_entry_size") cfg.record_cache_entry_size = std::stoull(val);
        else if (key == "zipf_theta")              cfg.zipf_theta              = std::stod(val);
        else if (key == "working_set_gib")         cfg.working_set_gib         = std::stod(val);
        else if (key == "record_size_bytes")       cfg.record_size_bytes       = std::stoull(val);
        else if (key == "page_size_bytes")         cfg.page_size_bytes         = std::stoull(val);
        else if (key == "sample_count")            cfg.sample_count            = std::stoull(val);
        else if (key == "seed")                    cfg.seed                    = std::stoull(val);
        else if (key == "skew_top1_threshold")     cfg.skew_top1_threshold     = std::stod(val);
        else throw std::invalid_argument("unknown arg: --" + key);
    }
    return cfg;
}

// ============================================================
// SplitMix64 RNG
// ============================================================
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

// ============================================================
// FNV Hash for scrambling
// ============================================================
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

// ============================================================
// Zipf Generator (returns rank in [1, n])
// ============================================================
class ZipfGenerator {
public:
    ZipfGenerator(u64 n, double theta) : n_(n), theta_(theta) {
        if (n < 2) throw std::invalid_argument("n must be >= 2");
        alpha_  = 1.0 / (1.0 - theta);
        zetan_  = zeta(n, theta);
        eta_    = (1.0 - std::pow(2.0 / n, 1.0 - theta)) /
                  (1.0 - zeta(2, theta) / zetan_);
    }
    u64 next(SplitMix64& rng) const {
        const double u  = rng.next01();
        const double uz = u * zetan_;
        if (uz < 1.0) return 1;
        if (uz < (1.0 + std::pow(0.5, theta_))) return 2;
        return 1 + static_cast<u64>(
            static_cast<double>(n_) * std::pow(eta_ * u - eta_ + 1.0, alpha_));
    }
private:
    static double zeta(u64 n, double theta) {
        double s = 0.0;
        for (u64 i = 1; i <= n; i++) s += std::pow(1.0 / i, theta);
        return s;
    }
    u64 n_; double theta_, alpha_, zetan_, eta_;
};

// ============================================================
// ScrambledZipf: rank -> fnv scramble -> uniform over [0, n)
// ============================================================
class ScrambledZipfGenerator {
public:
    ScrambledZipfGenerator(u64 n, double theta) : n_(n), zipf_(n, theta) {}
    u64 next(SplitMix64& rng) const {
        return fnvHash64(zipf_.next(rng)) % n_;
    }
private:
    u64 n_;
    ZipfGenerator zipf_;
};

// ============================================================
// Theoretical Hit Ratio Calculator
// ============================================================
struct HitRatioResult {
    // --- RecordCache coverage ---
    double theoretical_hit_ratio      = 0.0;  // pure Zipfian top-K coverage

    // --- Page classification ---
    u64    total_visited_pages        = 0;
    u64    skew_pages                 = 0;
    u64    cold_pages                 = 0;
    double skew_page_traffic_pct      = 0.0;
    double cold_page_traffic_pct      = 0.0;

    // --- Slot dominance ---
    double avg_top1_slot_ratio        = 0.0;  // avg (max_slot / page_hits) over visited pages
    double median_top1_slot_ratio     = 0.0;

    // --- RecordCache capacity ---
    u64    record_cache_capacity      = 0;
    u64    total_records              = 0;
    double coverage_pct               = 0.0;  // capacity / total_records
};

// ============================================================
// Main analysis
// ============================================================
HitRatioResult analyze(const Config& cfg) {
    const u64 total_records    = cfg.total_records();
    const u64 total_pages      = cfg.total_pages();
    const u64 records_per_page = cfg.records_per_page();
    const u64 rc_capacity      = cfg.record_cache_capacity();

    HitRatioResult result;
    result.record_cache_capacity = rc_capacity;
    result.total_records         = total_records;
    result.coverage_pct          = (double)rc_capacity / total_records * 100.0;

    // ----------------------------------------------------------
    // Step 1: Theoretical hit ratio (pure Zipfian, no scramble)
    //   Top-K records under Zipf(theta) cover what fraction of traffic?
    //   hit_ratio = sum_{i=1}^{K} (1/i^theta) / sum_{i=1}^{N} (1/i^theta)
    //   K = min(rc_capacity, total_records)
    // ----------------------------------------------------------
    {
        std::cout << "\n[Step 1] Computing theoretical Zipfian hit ratio "
                  << "(K=" << rc_capacity << ", N=" << total_records << ")...\n";
        // For large N this is slow; we approximate with partial sums
        // Use first min(total_records, 2e7) terms, then tail approximation
        const u64 exact_limit = std::min(total_records, u64{20'000'000});
        double zeta_n = 0.0, zeta_k = 0.0;
        const u64 K = std::min(rc_capacity, total_records);
        for (u64 i = 1; i <= exact_limit; i++) {
            double term = std::pow(1.0 / i, cfg.zipf_theta);
            zeta_n += term;
            if (i <= K) zeta_k += term;
        }
        // Tail approximation for i > exact_limit: integral (x^-theta) from L to N
        if (total_records > exact_limit) {
            double L = (double)exact_limit;
            double N = (double)total_records;
            double exp = 1.0 - cfg.zipf_theta;
            double tail = (std::pow(N + 0.5, exp) - std::pow(L + 0.5, exp)) / exp;
            zeta_n += tail;
            if (K > exact_limit) {
                double Kd = (double)K;
                double tail_k = (std::pow(Kd + 0.5, exp) - std::pow(L + 0.5, exp)) / exp;
                zeta_k += tail_k;
            }
        }
        result.theoretical_hit_ratio = zeta_k / zeta_n;
        std::cout << "  zeta_N = " << zeta_n << "\n";
        std::cout << "  zeta_K = " << zeta_k << "\n";
        std::cout << "  Theoretical hit ratio (pure Zipf top-K) = "
                  << std::fixed << std::setprecision(4)
                  << result.theoretical_hit_ratio * 100.0 << "%\n";
    }

    // ----------------------------------------------------------
    // Sampling only for page-shape classification
    // ----------------------------------------------------------
    ScrambledZipfGenerator gen(total_records, cfg.zipf_theta);
    SplitMix64 rng(cfg.seed);
    std::vector<u64> page_hits(total_pages, 0);
    std::vector<u64> page_max_slot_hit(total_pages, 0);
    std::vector<u64> page_slot_hits(total_records, 0);

    std::cout << "\n[Step 2] Sampling page/slot accesses for classification "
              << "(sample_count=" << cfg.sample_count << ")...\n";
    for (u64 i = 0; i < cfg.sample_count; i++) {
        const u64 record_id = gen.next(rng);
        const u64 page_id   = record_id / records_per_page;
        page_hits[page_id]++;
        page_slot_hits[record_id]++;
        if (page_slot_hits[record_id] > page_max_slot_hit[page_id]) {
            page_max_slot_hit[page_id] = page_slot_hits[record_id];
        }
    }
    std::cout << "  Sampling done.\n";

    // ----------------------------------------------------------
    // Step 4: Page classification
    //   SKEW_PAGE : visited && max_slot_ratio >= skew_top1_threshold
    //   COLD_PAGE : unvisited or max_slot_ratio < threshold
    //   (In ScrambledZipf, HOT_PAGE should be ~0%)
    // ----------------------------------------------------------
    std::cout << "\n[Step 4] Classifying pages...\n";

    u64 skew_traffic = 0, cold_traffic = 0;
    u64 skew_pages   = 0, cold_pages   = 0;
    u64 visited_pages = 0;

    std::vector<double> top1_ratios; // for median calculation
    top1_ratios.reserve(total_pages / 4);

    for (u64 page = 0; page < total_pages; page++) {
        const u64 ph = page_hits[page];
        if (ph == 0) {
            cold_pages++;
            cold_traffic += 0;
            continue;
        }
        visited_pages++;
        const double top1_ratio =
            static_cast<double>(page_max_slot_hit[page]) / static_cast<double>(ph);
        top1_ratios.push_back(top1_ratio);

        if (top1_ratio >= cfg.skew_top1_threshold) {
            skew_pages++;
            skew_traffic += ph;
        } else {
            // visited but not skewed enough -> treat as cold for RecordCache purpose
            cold_pages++;
            cold_traffic += ph;
        }
    }

    result.total_visited_pages   = visited_pages;
    result.skew_pages            = skew_pages;
    result.cold_pages            = cold_pages;
    result.skew_page_traffic_pct =
        (double)skew_traffic / cfg.sample_count * 100.0;
    result.cold_page_traffic_pct =
        (double)cold_traffic / cfg.sample_count * 100.0;

    // avg top1 ratio
    if (!top1_ratios.empty()) {
        result.avg_top1_slot_ratio =
            std::accumulate(top1_ratios.begin(), top1_ratios.end(), 0.0)
            / top1_ratios.size();
        std::sort(top1_ratios.begin(), top1_ratios.end());
        result.median_top1_slot_ratio =
            top1_ratios[top1_ratios.size() / 2];
    }

    std::cout << "  visited_pages = " << visited_pages
              << " / " << total_pages << "\n";
    std::cout << "  skew_pages    = " << skew_pages
              << "  (" << std::fixed << std::setprecision(2)
              << (double)skew_pages / total_pages * 100.0 << "% of total)\n";
    std::cout << "  cold_pages    = " << cold_pages
              << "  (" << std::fixed << std::setprecision(2)
              << (double)cold_pages / total_pages * 100.0 << "% of total)\n";

    return result;
}

// ============================================================
// Print final report
// ============================================================
void printReport(const Config& cfg, const HitRatioResult& r) {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║         Two-Level-Admission-Control Theoretical Report       ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";

    std::cout << "║  [Input]                                                     ║\n";
    std::cout << "║   working_set          : " << std::setw(6) << cfg.working_set_gib
              << " GiB"
              << std::setw(28) << "║\n";
    std::cout << "║   record_cache         : " << std::setw(6) << cfg.record_cache_gib
              << " GiB"
              << std::setw(28) << "║\n";
    std::cout << "║   record_size          : " << std::setw(6) << cfg.record_size_bytes
              << " B  "
              << std::setw(28) << "║\n";
    std::cout << "║   record_cache_entry   : " << std::setw(6) << cfg.record_cache_entry_size
              << " B  "
              << std::setw(28) << "║\n";
    std::cout << "║   zipf_theta           : " << std::setw(6) << std::fixed
              << std::setprecision(2) << cfg.zipf_theta
              << "    "
              << std::setw(28) << "║\n";
    std::cout << "║   total_records        : " << std::setw(12) << r.total_records
              << std::setw(22) << "║\n";
    std::cout << "║   record_cache_capacity: " << std::setw(12) << r.record_cache_capacity
              << std::setw(22) << "║\n";
    std::cout << "║   coverage_ratio       : " << std::setw(9) << std::fixed
              << std::setprecision(2) << r.coverage_pct << "%"
              << std::setw(25) << "║\n";

    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  [Theoretical Hit Ratio - Pure Zipfian Top-K]               ║\n";
    std::cout << "║   If RecordCache perfectly caches top-K records:            ║\n";
    std::cout << "║   hit_ratio = Σ(1/i^θ, i=1..K) / Σ(1/i^θ, i=1..N)        ║\n";
    std::cout << "║                                                              ║\n";
    std::cout << "║   >> Theoretical Upper Bound : " << std::setw(8) << std::fixed
              << std::setprecision(2) << r.theoretical_hit_ratio * 100.0
              << " %                    ║\n";

    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  [Page Classification - ScrambledZipf]                      ║\n";
    std::cout << "║   (HOT_PAGE should be ~0% under ScrambledZipf)              ║\n";
    std::cout << "║                                                              ║\n";
    std::cout << "║   visited pages        : " << std::setw(12) << r.total_visited_pages
              << std::setw(22) << "║\n";
    std::cout << "║   SKEW_PAGE count      : " << std::setw(12) << r.skew_pages
              << std::setw(22) << "║\n";
    std::cout << "║   COLD_PAGE count      : " << std::setw(12) << r.cold_pages
              << std::setw(22) << "║\n";
    std::cout << "║   SKEW traffic         : " << std::setw(8) << std::fixed
              << std::setprecision(2) << r.skew_page_traffic_pct
              << " %"  << std::setw(24) << "║\n";
    std::cout << "║   COLD traffic         : " << std::setw(8) << std::fixed
              << std::setprecision(2) << r.cold_page_traffic_pct
              << " %"  << std::setw(24) << "║\n";
    std::cout << "║   avg  top1_slot_ratio : " << std::setw(8) << std::fixed
              << std::setprecision(2) << r.avg_top1_slot_ratio * 100.0
              << " %"  << std::setw(24) << "║\n";
    std::cout << "║   median top1_slot_ratio: " << std::setw(7) << std::fixed
              << std::setprecision(2) << r.median_top1_slot_ratio * 100.0
              << " %"  << std::setw(24) << "║\n";

    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  [Interpretation]                                            ║\n";

    if (r.skew_pages == 0) {
        std::cout << "║   WARNING: No SKEW pages detected.                          ║\n";
        std::cout << "║   Check skew_top1_threshold or sample_count.               ║\n";
    } else if (r.skew_page_traffic_pct < 50.0) {
        std::cout << "║   WARNING: SKEW traffic < 50%, workload may not be         ║\n";
        std::cout << "║   skewed enough for RecordCache to be effective.            ║\n";
    } else {
        std::cout << "║   OK: SKEW traffic dominates. RecordCache is well-suited.  ║\n";
        std::cout << "║   Theoretical hit ratio is the ceiling your system         ║\n";
        std::cout << "║   should approach if admission control works correctly.     ║\n";
    }

    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
}

// ============================================================
// Main
// ============================================================
int main(int argc, char** argv) {
    try {
        const Config cfg = parseArgs(argc, argv);

        if (cfg.records_per_page() == 0)
            throw std::invalid_argument(
                "records_per_page = 0: page_size_bytes too small or record_size_bytes too large");
        if (cfg.total_records() < 2)
            throw std::invalid_argument("total_records must be >= 2");

        cfg.print();

        const HitRatioResult result = analyze(cfg);
        printReport(cfg, result);

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nError: " << ex.what() << "\n\n";
        std::cerr << "Usage:\n";
        std::cerr << "  ./pure_record_cache_data_generator \\\n";
        std::cerr << "      --record_cache_gib=1        \\ # RecordCache size\n";
        std::cerr << "      --record_cache_entry_size=100\\ # bytes per cached entry\n";
        std::cerr << "      --working_set_gib=4          \\ # total data size\n";
        std::cerr << "      --record_size_bytes=100      \\ # bytes per record\n";
        std::cerr << "      --page_size_bytes=16384      \\ # bytes per page\n";
        std::cerr << "      --zipf_theta=0.99            \\ # Zipf skew\n";
        std::cerr << "      --sample_count=10000000      \\ # simulation samples\n";
        std::cerr << "      --skew_top1_threshold=0.10     # skew detection threshold\n";
        return 1;
    }
}
