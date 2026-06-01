#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using u64 = std::uint64_t;

constexpr u64 GiB = 1024ULL * 1024ULL * 1024ULL;

struct Config {
    double test_load_gib = 4.0;
    u64 record_cache_entry_size = 16;
    double theta = 0.99;
    u64 sample_count = 10'000'000ULL;
    u64 seed = 42;
    u64 page_size_bytes = 16 * 1024;
    double hot_page_percent = 5.0;
    double skew_top1_ratio = 0.30;
    bool print_zero_slots = false;
    std::string page_csv_path = "zipfian_page_hits.csv";
    std::string slot_csv_path = "zipfian_slot_hits.csv";
};

bool parseBool(const std::string& value) {
    if (value == "1" || value == "true" || value == "TRUE" || value == "True") return true;
    if (value == "0" || value == "false" || value == "FALSE" || value == "False") return false;
    throw std::invalid_argument("invalid bool value: " + value);
}

Config parseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg.rfind("--", 0) != 0) {
            throw std::invalid_argument("invalid arg format: " + arg);
        }
        const auto eq = arg.find('=');
        if (eq == std::string::npos) {
            throw std::invalid_argument("arg must be --key=value: " + arg);
        }
        const std::string key = arg.substr(2, eq - 2);
        const std::string val = arg.substr(eq + 1);

        if (key == "test_load_gib") cfg.test_load_gib = std::stod(val);
        else if (key == "record_cache_entry_size") cfg.record_cache_entry_size = std::stoull(val);
        else if (key == "theta") cfg.theta = std::stod(val);
        else if (key == "sample_count") cfg.sample_count = std::stoull(val);
        else if (key == "seed") cfg.seed = std::stoull(val);
        else if (key == "page_size_bytes") cfg.page_size_bytes = std::stoull(val);
        else if (key == "hot_page_percent") cfg.hot_page_percent = std::stod(val);
        else if (key == "skew_top1_ratio") cfg.skew_top1_ratio = std::stod(val);
        else if (key == "print_zero_slots") cfg.print_zero_slots = parseBool(val);
        else if (key == "page_csv_path") cfg.page_csv_path = val;
        else if (key == "slot_csv_path") cfg.slot_csv_path = val;
        else throw std::invalid_argument("unknown arg: --" + key);
    }
    return cfg;
}

void validateConfig(const Config& cfg) {
    if (cfg.test_load_gib <= 0.0) throw std::invalid_argument("test_load_gib must be > 0");
    if (cfg.record_cache_entry_size == 0) throw std::invalid_argument("record_cache_entry_size must be > 0");
    if (cfg.page_size_bytes == 0) throw std::invalid_argument("page_size_bytes must be > 0");
    if (cfg.theta <= 0.0 || cfg.theta >= 1.0) throw std::invalid_argument("theta must be in (0, 1)");
    if (cfg.sample_count == 0) throw std::invalid_argument("sample_count must be > 0");
    if (cfg.hot_page_percent < 0.0 || cfg.hot_page_percent > 100.0) {
        throw std::invalid_argument("hot_page_percent must be in [0, 100]");
    }
    if (cfg.skew_top1_ratio < 0.0 || cfg.skew_top1_ratio > 1.0) {
        throw std::invalid_argument("skew_top1_ratio must be in [0, 1]");
    }
}

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
        const long double inv = 1.0L / static_cast<long double>(std::numeric_limits<u64>::max());
        return static_cast<double>(static_cast<long double>(next()) * inv);
    }

private:
    u64 state_;
};

u64 fnvHash64(u64 val) {
    constexpr u64 FNV_OFFSET_BASIS_64 = 0xCBF29CE484222325ULL;
    constexpr u64 FNV_PRIME_64 = 1099511628211ULL;
    u64 hash_val = FNV_OFFSET_BASIS_64;
    for (int i = 0; i < 8; i++) {
        const u64 octet = val & 0x00ffULL;
        val >>= 8;
        hash_val ^= octet;
        hash_val *= FNV_PRIME_64;
    }
    return hash_val;
}

class ZipfGenerator {
public:
    ZipfGenerator(u64 item_count, double theta) : n_(item_count), theta_(theta) {
        if (n_ < 2) throw std::invalid_argument("item_count must be >= 2");
        alpha_ = 1.0 / (1.0 - theta_);
        zetan_ = zeta(n_, theta_);
        eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n_), 1.0 - theta_)) /
               (1.0 - zeta(2, theta_) / zetan_);
    }

    u64 next(SplitMix64& rng) const {
        const double u = rng.next01();
        const double uz = u * zetan_;
        if (uz < 1.0) return 1;
        if (uz < (1.0 + std::pow(0.5, theta_))) return 2;
        const double inner = eta_ * u - eta_ + 1.0;
        return 1 + static_cast<u64>(static_cast<double>(n_) * std::pow(inner, alpha_));
    }

private:
    static double zeta(u64 n, double theta) {
        double sum = 0.0;
        for (u64 i = 1; i <= n; i++) {
            sum += std::pow(1.0 / static_cast<double>(i), theta);
        }
        return sum;
    }

    u64 n_;
    double theta_;
    double alpha_;
    double zetan_;
    double eta_;
};

class ScrambledZipfGenerator {
public:
    ScrambledZipfGenerator(u64 min, u64 max, double theta)
        : min_(min), max_(max), n_(max - min), zipf_(n_, theta) {
        if (max_ <= min_) throw std::invalid_argument("max must be > min");
    }

    u64 next(SplitMix64& rng) const {
        const u64 zipf_value = zipf_.next(rng);
        return min_ + (fnvHash64(zipf_value) % n_);
    }

private:
    u64 min_;
    u64 max_;
    u64 n_;
    ZipfGenerator zipf_;
};

enum class PageType : std::uint8_t {
    HOT_PAGE,
    SKEW_PAGE,
    COLD_PAGE
};

std::string toString(PageType type) {
    switch (type) {
        case PageType::HOT_PAGE: return "HOT_PAGE";
        case PageType::SKEW_PAGE: return "SKEW_PAGE";
        case PageType::COLD_PAGE: return "COLD_PAGE";
    }
    return "UNKNOWN";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Config cfg = parseArgs(argc, argv);
        validateConfig(cfg);

        const u64 records_per_page = cfg.page_size_bytes / cfg.record_cache_entry_size / 2;
        if (records_per_page == 0) {
            throw std::invalid_argument("records_per_page becomes 0, decrease record_cache_entry_size");
        }

        const long double total_bytes_ld = static_cast<long double>(cfg.test_load_gib) * static_cast<long double>(GiB);
        const u64 total_bytes = static_cast<u64>(total_bytes_ld);
        const u64 total_pages = total_bytes / cfg.page_size_bytes;
        if (total_pages == 0) throw std::invalid_argument("total_pages becomes 0");

        const u64 total_records = total_pages * records_per_page;
        if (total_records < 2) throw std::invalid_argument("total_records must be >= 2");

        std::cout << "=== Configuration ===\n";
        std::cout << "test_load_gib            = " << cfg.test_load_gib << "\n";
        std::cout << "record_cache_entry_size  = " << cfg.record_cache_entry_size << " B\n";
        std::cout << "page_size_bytes          = " << cfg.page_size_bytes << " B\n";
        std::cout << "records_per_page         = " << records_per_page << " (16KB / entry / 2)\n";
        std::cout << "total_pages              = " << total_pages << "\n";
        std::cout << "total_records            = " << total_records << "\n";
        std::cout << "theta                    = " << cfg.theta << "\n";
        std::cout << "sample_count             = " << cfg.sample_count << "\n";
        std::cout << "hot_page_percent         = " << cfg.hot_page_percent << "%\n";
        std::cout << "skew_top1_ratio          = " << cfg.skew_top1_ratio << "\n";
        std::cout << "page_csv_path            = " << cfg.page_csv_path << "\n";
        std::cout << "slot_csv_path            = " << cfg.slot_csv_path << "\n";
        std::cout << "print_zero_slots         = " << (cfg.print_zero_slots ? "true" : "false") << "\n\n";

        ScrambledZipfGenerator generator(0, total_records, cfg.theta);
        SplitMix64 rng(cfg.seed);

        std::vector<u64> page_hits(total_pages, 0);
        std::vector<std::unordered_map<u64, u64>> slot_hits(total_pages);

        for (u64 i = 0; i < cfg.sample_count; i++) {
            const u64 key = generator.next(rng);
            const u64 page = key / records_per_page;
            const u64 slot = key % records_per_page;
            page_hits[page]++;
            slot_hits[page][slot]++;
        }

        std::vector<u64> page_order(total_pages);
        std::iota(page_order.begin(), page_order.end(), 0ULL);
        std::sort(page_order.begin(), page_order.end(), [&](u64 a, u64 b) {
            return page_hits[a] > page_hits[b];
        });

        const u64 hot_page_count = static_cast<u64>(
            std::ceil(static_cast<long double>(total_pages) * cfg.hot_page_percent / 100.0L));

        std::vector<PageType> page_types(total_pages, PageType::COLD_PAGE);
        for (u64 i = 0; i < std::min(hot_page_count, total_pages); i++) {
            if (page_hits[page_order[i]] > 0) page_types[page_order[i]] = PageType::HOT_PAGE;
        }

        for (u64 page = 0; page < total_pages; page++) {
            if (page_types[page] == PageType::HOT_PAGE) continue;
            if (page_hits[page] == 0) {
                page_types[page] = PageType::COLD_PAGE;
                continue;
            }
            u64 max_slot_hit = 0;
            for (const auto& [slot, hit] : slot_hits[page]) {
                (void)slot;
                if (hit > max_slot_hit) max_slot_hit = hit;
            }
            const double top1_ratio = static_cast<double>(max_slot_hit) / static_cast<double>(page_hits[page]);
            page_types[page] = (top1_ratio >= cfg.skew_top1_ratio) ? PageType::SKEW_PAGE : PageType::COLD_PAGE;
        }

        u64 hot_pages = 0, skew_pages = 0, cold_pages = 0;
        u64 hot_traffic = 0, skew_traffic = 0, cold_traffic = 0;
        for (u64 page = 0; page < total_pages; page++) {
            switch (page_types[page]) {
                case PageType::HOT_PAGE:
                    hot_pages++;
                    hot_traffic += page_hits[page];
                    break;
                case PageType::SKEW_PAGE:
                    skew_pages++;
                    skew_traffic += page_hits[page];
                    break;
                case PageType::COLD_PAGE:
                    cold_pages++;
                    cold_traffic += page_hits[page];
                    break;
            }
        }

        std::ofstream page_csv(cfg.page_csv_path);
        if (!page_csv) throw std::runtime_error("cannot open page csv path: " + cfg.page_csv_path);
        page_csv << "page_id,page_hits,page_hit_ratio,page_type,max_slot_hits,max_slot_ratio,active_slots\n";
        for (u64 page = 0; page < total_pages; page++) {
            const u64 ph = page_hits[page];
            u64 max_slot_hit = 0;
            for (const auto& [slot, hit] : slot_hits[page]) {
                (void)slot;
                if (hit > max_slot_hit) max_slot_hit = hit;
            }
            const double page_ratio = static_cast<double>(ph) / static_cast<double>(cfg.sample_count);
            const double max_slot_ratio =
                (ph == 0) ? 0.0 : (static_cast<double>(max_slot_hit) / static_cast<double>(ph));
            page_csv << page << ","
                     << ph << ","
                     << std::fixed << std::setprecision(8) << page_ratio << ","
                     << toString(page_types[page]) << ","
                     << max_slot_hit << ","
                     << std::fixed << std::setprecision(8) << max_slot_ratio << ","
                     << slot_hits[page].size() << "\n";
        }
        page_csv.close();

        std::ofstream slot_csv(cfg.slot_csv_path);
        if (!slot_csv) throw std::runtime_error("cannot open slot csv path: " + cfg.slot_csv_path);
        slot_csv << "page_id,slot_id,slot_hits,slot_ratio_within_page,page_type\n";
        for (u64 page = 0; page < total_pages; page++) {
            const u64 ph = page_hits[page];
            if (cfg.print_zero_slots) {
                for (u64 slot = 0; slot < records_per_page; slot++) {
                    const auto it = slot_hits[page].find(slot);
                    const u64 hit = (it == slot_hits[page].end()) ? 0 : it->second;
                    const double ratio = (ph == 0) ? 0.0 : (static_cast<double>(hit) / static_cast<double>(ph));
                    slot_csv << page << ","
                             << slot << ","
                             << hit << ","
                             << std::fixed << std::setprecision(8) << ratio << ","
                             << toString(page_types[page]) << "\n";
                }
            } else {
                for (const auto& [slot, hit] : slot_hits[page]) {
                    const double ratio = (ph == 0) ? 0.0 : (static_cast<double>(hit) / static_cast<double>(ph));
                    slot_csv << page << ","
                             << slot << ","
                             << hit << ","
                             << std::fixed << std::setprecision(8) << ratio << ","
                             << toString(page_types[page]) << "\n";
                }
            }
        }
        slot_csv.close();

        std::cout << "=== Classification Summary ===\n";
        std::cout << "HOT_PAGE:  " << hot_pages << " pages, traffic " << std::fixed << std::setprecision(2)
                  << (static_cast<double>(hot_traffic) / static_cast<double>(cfg.sample_count) * 100.0) << "%\n";
        std::cout << "SKEW_PAGE: " << skew_pages << " pages, traffic " << std::fixed << std::setprecision(2)
                  << (static_cast<double>(skew_traffic) / static_cast<double>(cfg.sample_count) * 100.0) << "%\n";
        std::cout << "COLD_PAGE: " << cold_pages << " pages, traffic " << std::fixed << std::setprecision(2)
                  << (static_cast<double>(cold_traffic) / static_cast<double>(cfg.sample_count) * 100.0) << "%\n\n";

        std::cout << "Wrote page stats to: " << cfg.page_csv_path << "\n";
        std::cout << "Wrote slot stats to: " << cfg.slot_csv_path << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        std::cerr << "Usage example:\n";
        std::cerr << "  ./experiment_zipfian_data_generator "
                  << "--test_load_gib=4 "
                  << "--record_cache_entry_size=16 "
                  << "--theta=0.99 "
                  << "--sample_count=10000000 "
                  << "--hot_page_percent=5 "
                  << "--skew_top1_ratio=0.30 "
                  << "--print_zero_slots=false\n";
        return 1;
    }
}
