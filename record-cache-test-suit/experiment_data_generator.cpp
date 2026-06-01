#include <vector>
#include <random>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <iomanip>
#include <unordered_map>
#include <string>
#include <getopt.h>

// ============================================================
// Config
// ============================================================
struct Config {
    uint64_t test_load_gib      = 4;
    uint64_t record_size_bytes  = 100;
    uint64_t page_size_bytes    = 16384;
    double   zipf_theta         = 0.99;
    uint64_t sample_count       = 5'000'000;
    uint64_t seed               = 42;

    uint64_t records_per_page() const {
        return page_size_bytes / record_size_bytes;
    }
    uint64_t total_records() const {
        return test_load_gib * 1024ULL * 1024 * 1024 / record_size_bytes;
    }
    uint64_t total_pages() const {
        uint64_t rpp = records_per_page();
        return (total_records() + rpp - 1) / rpp;
    }

    void print() const {
        std::cout << "\n=== Configuration ===\n";
        std::cout << "  test_load_gib      : " << test_load_gib     << " GiB\n";
        std::cout << "  record_size_bytes  : " << record_size_bytes  << " B\n";
        std::cout << "  page_size_bytes    : " << page_size_bytes    << " B\n";
        std::cout << "  records_per_page   : " << records_per_page() << "\n";
        std::cout << "  total_records      : " << total_records()    << "\n";
        std::cout << "  total_pages        : " << total_pages()      << "\n";
        std::cout << "  zipf_theta         : " << zipf_theta         << "\n";
        std::cout << "  sample_count       : " << sample_count       << "\n";
        std::cout << "  seed               : " << seed               << "\n";
    }
};

// ============================================================
// Scrambled Zipf Generator (matches YCSB ScrambledZipfGenerator)
// ============================================================
class ScrambledZipfGenerator {
    uint64_t n_;
    double   theta_;
    double   alpha_;
    double   zetan_;
    double   eta_;
    std::mt19937_64 rng_;

    double zeta(uint64_t n, double theta) {
        double sum = 0.0;
        for (uint64_t i = 1; i <= n; i++)
            sum += 1.0 / std::pow((double)i, theta);
        return sum;
    }

public:
    void init(uint64_t n, double theta, uint64_t seed = 42) {
        n_      = n;
        theta_  = theta;
        rng_    = std::mt19937_64(seed);
        zetan_  = zeta(n, theta);
        alpha_  = 1.0 / (1.0 - theta);
        eta_    = (1.0 - std::pow(2.0 / n, 1.0 - theta))
                / (1.0 - zeta(2, theta) / zetan_);
    }

    uint64_t next() {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        double u_val = u(rng_);
        double uz    = u_val * zetan_;

        uint64_t rank;
        if (uz < 1.0)
            rank = 0;
        else if (uz < 1.0 + std::pow(0.5, theta_))
            rank = 1;
        else
            rank = (uint64_t)(n_ * std::pow(eta_ * u_val - eta_ + 1.0, alpha_));

        if (rank >= n_) rank = n_ - 1;

        // Scramble: break spatial locality (same as YCSB)
        uint64_t key = rank;
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        key *= 0xc4ceb9fe1a85ec53ULL;
        key ^= key >> 33;
        key = key % n_;
        return key;
    }
};

// ============================================================
// Page type
// ============================================================
enum class PageType { HOT, SKEW, COLD };

std::string page_type_str(PageType t) {
    switch (t) {
        case PageType::HOT:  return "HOT";
        case PageType::SKEW: return "SKEW";
        case PageType::COLD: return "COLD";
    }
    return "";
}

// ============================================================
// Per-page statistics
// ============================================================
struct PageStat {
    uint64_t page_id;
    uint64_t total_visits;
    std::vector<uint64_t> slot_visits;  // indexed by slot_id, sorted after build

    struct SlotEntry {
        uint64_t slot_id;
        uint64_t visits;
    };
    std::vector<SlotEntry> sorted_slots;  // sorted desc by visits

    void build_sorted_slots() {
        sorted_slots.clear();
        for (uint64_t s = 0; s < slot_visits.size(); s++) {
            if (slot_visits[s] > 0)
                sorted_slots.push_back({s, slot_visits[s]});
        }
        std::sort(sorted_slots.begin(), sorted_slots.end(),
                  [](const SlotEntry& a, const SlotEntry& b) {
                      return a.visits > b.visits;
                  });
    }

    double top_k_ratio(uint64_t k) const {
        if (total_visits == 0) return 0.0;
        uint64_t sum = 0;
        for (uint64_t i = 0; i < std::min(k, (uint64_t)sorted_slots.size()); i++)
            sum += sorted_slots[i].visits;
        return (double)sum / total_visits;
    }
};

// ============================================================
// Analysis
// ============================================================
void run_analysis(const Config& cfg) {
    cfg.print();

    const uint64_t total_records    = cfg.total_records();
    const uint64_t total_pages      = cfg.total_pages();
    const uint64_t records_per_page = cfg.records_per_page();

    // Build generator
    // Note: zeta(n) for large n is slow; print progress
    std::cout << "\nInitializing ScrambledZipf (n=" << total_records
              << ", theta=" << cfg.zipf_theta << ")...\n";
    ScrambledZipfGenerator gen;
    gen.init(total_records, cfg.zipf_theta, cfg.seed);
    std::cout << "Generator ready.\n";

    // Allocate counters
    // page_slot_hits[page * records_per_page + slot]
    std::vector<uint64_t> page_slot_hits(total_pages * records_per_page, 0);
    std::vector<uint64_t> page_hits(total_pages, 0);

    std::cout << "Sampling " << cfg.sample_count << " accesses...\n";
    for (uint64_t i = 0; i < cfg.sample_count; i++) {
        uint64_t key  = gen.next();
        uint64_t page = key / records_per_page;
        uint64_t slot = key % records_per_page;
        assert(page < total_pages);
        page_hits[page]++;
        page_slot_hits[page * records_per_page + slot]++;
    }
    std::cout << "Sampling done.\n";

    // Build PageStat for visited pages only
    std::vector<PageStat> stats;
    stats.reserve(total_pages);
    for (uint64_t p = 0; p < total_pages; p++) {
        if (page_hits[p] == 0) continue;
        PageStat ps;
        ps.page_id      = p;
        ps.total_visits = page_hits[p];
        ps.slot_visits.resize(records_per_page);
        for (uint64_t s = 0; s < records_per_page; s++)
            ps.slot_visits[s] = page_slot_hits[p * records_per_page + s];
        ps.build_sorted_slots();
        stats.push_back(std::move(ps));
    }

    // Sort pages desc by total_visits
    std::sort(stats.begin(), stats.end(),
              [](const PageStat& a, const PageStat& b) {
                  return a.total_visits > b.total_visits;
              });

    // Classification thresholds
    double avg_visits    = (double)cfg.sample_count / total_pages;
    double hot_threshold = avg_visits * 3.0;
    // top 5% slots of a page
    uint64_t top5_slots  = std::max(uint64_t{1},
                                     (uint64_t)(records_per_page * 0.05));
    double skew_ratio    = 0.7;

    // [Fix]: fix bug about hot page and skew page distinction.
    auto classify = [&](const PageStat& ps) -> PageType {
        double max_slot_ratio = ps.sorted_slots.empty() ? 0.0 :
        (double)ps.sorted_slots[0].visits / ps.total_visits;

        // if any single slot exceeds 10% -> it is skew, no matter how high the page visit
        if(max_slot_ratio >= 0.10) return PageType::SKEW;
        if((double)ps.total_visits >= hot_threshold) return PageType::HOT;  // No dominance
        return PageType::COLD;
    };

    // Aggregate per type
    struct TypeAgg {
        uint64_t page_count   = 0;
        uint64_t total_visits = 0;
    };
    TypeAgg agg_hot, agg_skew, agg_cold;
    for (auto& ps : stats) {
        auto& a = (classify(ps) == PageType::HOT)  ? agg_hot
                : (classify(ps) == PageType::SKEW) ? agg_skew
                                                   : agg_cold;
        a.page_count++;
        a.total_visits += ps.total_visits;
    }
    // unvisited pages all count as COLD
    uint64_t unvisited = total_pages - (uint64_t)stats.size();
    agg_cold.page_count += unvisited;

    auto traffic_pct = [&](uint64_t v) {
        return (double)v / cfg.sample_count * 100.0;
    };

    // --------------------------------------------------------
    // Summary
    // --------------------------------------------------------
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n=== Page Type Summary ===\n";
    std::cout << "  avg_visits_per_page : " << avg_visits    << "\n";
    std::cout << "  hot_threshold       : " << hot_threshold << " (3x avg)\n";
    std::cout << "  top5_slots          : " << top5_slots
              << " / " << records_per_page << " slots\n";
    std::cout << "  skew_slot_ratio     : " << skew_ratio * 100.0 << "%\n\n";

    std::cout << std::setw(8)  << "Type"
              << std::setw(12) << "Pages"
              << std::setw(14) << "Visits"
              << std::setw(14) << "Traffic%"
              << "\n"
              << std::string(48, '-') << "\n";

    for (auto& [label, agg] : std::vector<std::pair<std::string, TypeAgg>>{
            {"HOT",  agg_hot},
            {"SKEW", agg_skew},
            {"COLD", agg_cold}}) {
        std::cout << std::setw(8)  << label
                  << std::setw(12) << agg.page_count
                  << std::setw(14) << agg.total_visits
                  << std::setw(13) << traffic_pct(agg.total_visits) << "%\n";
    }

    // --------------------------------------------------------
    // Per-page detail: top 50
    // --------------------------------------------------------
    uint64_t print_n = std::min((uint64_t)stats.size(), uint64_t{50});
    std::cout << "\n=== Per-Page Detail (top " << print_n << " pages by visits) ===\n";
    std::cout << std::setw(6)  << "Rank"
              << std::setw(10) << "PageID"
              << std::setw(10) << "Visits"
              << std::setw(11) << "Traffic%"
              << std::setw(8)  << "Type"
              << std::setw(11) << "Top1Slot%"
              << std::setw(11) << "Top5Slot%"
              << "  Top10 Slots [slot:visits]\n"
              << std::string(130, '-') << "\n";

    for (uint64_t r = 0; r < print_n; r++) {
        const PageStat& ps = stats[r];
        PageType t = classify(ps);

        std::cout << std::setw(6)  << r
                  << std::setw(10) << ps.page_id
                  << std::setw(10) << ps.total_visits
                  << std::setw(10) << traffic_pct(ps.total_visits) << "%"
                  << std::setw(8)  << page_type_str(t)
                  << std::setw(10) << ps.top_k_ratio(1) * 100.0           << "%"
                  << std::setw(10) << ps.top_k_ratio(top5_slots) * 100.0  << "%"
                  << "  [";

        uint64_t show = std::min((uint64_t)ps.sorted_slots.size(), uint64_t{10});
        for (uint64_t s = 0; s < show; s++) {
            if (s > 0) std::cout << ", ";
            std::cout << ps.sorted_slots[s].slot_id
                      << ":" << ps.sorted_slots[s].visits;
        }
        std::cout << "]\n";
    }

    // --------------------------------------------------------
    // SKEW page slot detail: first 5 SKEW pages
    // --------------------------------------------------------
    std::cout << "\n=== SKEW Page Slot Detail (first 5 SKEW pages) ===\n";
    uint64_t shown = 0;
    for (auto& ps : stats) {
        if (shown >= 5) break;
        if (classify(ps) != PageType::SKEW) continue;
        shown++;

        std::cout << "\n  PageID=" << ps.page_id
                  << "  TotalVisits=" << ps.total_visits
                  << "  Traffic=" << traffic_pct(ps.total_visits) << "%\n";
        std::cout << "  " << std::string(36, '-') << "\n";
        std::cout << "  " << std::setw(8)  << "SlotID"
                           << std::setw(10) << "Visits"
                           << std::setw(10) << "SlotPct%"
                           << "\n";

        for (auto& se : ps.sorted_slots) {
            if (se.visits == 0) break;
            double pct = (double)se.visits / ps.total_visits * 100.0;
            std::cout << "  " << std::setw(8)  << se.slot_id
                               << std::setw(10) << se.visits
                               << std::setw(9)  << pct << "%\n";
        }
    }
}

// ============================================================
// Argument parsing
// ============================================================
void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --test_load_gib=N       total data size in GiB (default: 4)\n"
              << "  --record_size_bytes=N   record size in bytes   (default: 100)\n"
              << "  --page_size_bytes=N     page size in bytes     (default: 16384)\n"
              << "  --zipf_theta=F          Zipf skewness          (default: 0.99)\n"
              << "  --sample_count=N        number of samples      (default: 5000000)\n"
              << "  --seed=N                random seed            (default: 42)\n";
}

int main(int argc, char** argv) {
    Config cfg;

    static struct option long_options[] = {
        {"test_load_gib",     required_argument, 0, 0},
        {"record_size_bytes", required_argument, 0, 0},
        {"page_size_bytes",   required_argument, 0, 0},
        {"zipf_theta",        required_argument, 0, 0},
        {"sample_count",      required_argument, 0, 0},
        {"seed",              required_argument, 0, 0},
        {"help",              no_argument,       0, 0},
        {0, 0, 0, 0}
    };

    int opt_index = 0;
    int c;
    while ((c = getopt_long(argc, argv, "", long_options, &opt_index)) != -1) {
        if (c != 0) continue;
        std::string name  = long_options[opt_index].name;
        std::string value = optarg ? optarg : "";

        if      (name == "test_load_gib")     cfg.test_load_gib     = std::stoull(value);
        else if (name == "record_size_bytes") cfg.record_size_bytes  = std::stoull(value);
        else if (name == "page_size_bytes")   cfg.page_size_bytes    = std::stoull(value);
        else if (name == "zipf_theta")        cfg.zipf_theta         = std::stod(value);
        else if (name == "sample_count")      cfg.sample_count       = std::stoull(value);
        else if (name == "seed")              cfg.seed               = std::stoull(value);
        else if (name == "help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (cfg.records_per_page() == 0) {
        std::cerr << "ERROR: records_per_page=0, "
                  << "page_size_bytes=" << cfg.page_size_bytes
                  << " record_size_bytes=" << cfg.record_size_bytes << "\n";
        return 1;
    }
    if (cfg.total_records() == 0) {
        std::cerr << "ERROR: total_records=0\n";
        return 1;
    }

    run_analysis(cfg);
    return 0;
}
