#include <iostream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <random>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <cassert>
#include <sstream>
#include<chrono>

// ============================================================
// 1. Zipfian Generator
// ============================================================
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

// ============================================================
// 2. 固定哈希：logical_record_id → (page_id, slot_id)
// ============================================================
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

// ============================================================
// 3. 统计结构
// ============================================================
struct PageStats {
    uint64_t total_accesses = 0;
    std::unordered_map<uint16_t, uint64_t> slot_accesses;
};

// ============================================================
// 4. 辅助：对某页的slot列表排序
// ============================================================
std::vector<std::pair<uint16_t, uint64_t>>
sorted_slot_list(const PageStats& ps) {
    std::vector<std::pair<uint16_t, uint64_t>> v(
        ps.slot_accesses.begin(), ps.slot_accesses.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b){ return a.second > b.second; });
    return v;
}

uint64_t topK_sum(const std::vector<std::pair<uint16_t,uint64_t>>& sv, int k) {
    uint64_t s = 0;
    for (int i = 0; i < k && i < (int)sv.size(); i++) s += sv[i].second;
    return s;
}

// ============================================================
// 5. 核心分析报告
// ============================================================
void analyze_and_report(
    const std::unordered_map<uint64_t, PageStats>& page_stats,
    uint16_t slots_per_page,
    uint64_t total_requests,
    uint64_t total_records,
    double   zipf_theta,
    std::ostream& out)
{
    // 按总访问次数降序排列
    std::vector<std::pair<uint64_t, const PageStats*>> sorted_pages;
    sorted_pages.reserve(page_stats.size());
    for (const auto& [pid, ps] : page_stats)
        sorted_pages.push_back({pid, &ps});
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const auto& a, const auto& b){
                  return a.second->total_accesses > b.second->total_accesses;
              });

    uint64_t total_page_access_sum = 0;
    for (const auto& [pid, ps] : page_stats)
        total_page_access_sum += ps.total_accesses;

    // ===========================================================
    // Section 0: 全局概览
    // ===========================================================
    out << "\n";
    out << "================================================================\n";
    out << " MOTIVATION EVIDENCE: Hot Pages Driven by Few Hot Records\n";
    out << "================================================================\n";
    out << " Total Requests      : " << total_requests        << "\n";
    out << " Total Records       : " << total_records         << "\n";
    out << " Zipfian Theta       : " << zipf_theta            << "\n";
    out << " Slots per Page      : " << slots_per_page        << "\n";
    out << " Pages Ever Accessed : " << page_stats.size()     << "\n";
    out << " Avg Accesses/Page   : "
        << std::fixed << std::setprecision(2)
        << (double)total_page_access_sum / (double)page_stats.size() << "\n";
    out << "================================================================\n";

    // ===========================================================
    // Section 1: Top-30 热页的 Slot Skew 概览表
    // ===========================================================
    out << "\n[Section 1: Top-30 Hottest Pages — Slot-Level Skew]\n";
    const int W = 120;
    out << std::string(W, '-') << "\n";
    out << std::left
        << std::setw(6)  << "Rank"
        << std::setw(12) << "PageID"
        << std::setw(12) << "TotalAcc"
        << std::setw(18) << "ActiveSlots"
        << std::setw(12) << "HotSlot"
        << std::setw(10) << "HotCnt"
        << std::setw(9)  << "Top1%"
        << std::setw(9)  << "Top3%"
        << std::setw(9)  << "Top5%"
        << "Verdict\n";
    out << std::string(W, '-') << "\n";

    int rank = 0;
    for (const auto& [pid, ps_ptr] : sorted_pages) {
        if (++rank > 30) break;
        const PageStats& ps = *ps_ptr;
        auto sv = sorted_slot_list(ps);

        double r1 = 100.0 * topK_sum(sv,1) / ps.total_accesses;
        double r3 = 100.0 * topK_sum(sv,3) / ps.total_accesses;
        double r5 = 100.0 * topK_sum(sv,5) / ps.total_accesses;

        std::string verdict;
        if      (r1 >= 60.0) verdict = "***  1 record dominates";
        else if (r3 >= 70.0) verdict = "**   3 records dominate";
        else if (r5 >= 80.0) verdict = "*    5 records dominate";
        else                 verdict = "     Relatively uniform";

        std::string active =
            std::to_string(ps.slot_accesses.size()) + "/" +
            std::to_string(slots_per_page);

        out << std::left
            << std::setw(6)  << rank
            << std::setw(12) << pid
            << std::setw(12) << ps.total_accesses
            << std::setw(18) << active
            << std::setw(12) << sv[0].first
            << std::setw(10) << sv[0].second
            << std::setw(9)  << std::fixed << std::setprecision(1) << r1
            << std::setw(9)  << r3
            << std::setw(9)  << r5
            << verdict << "\n";
    }

    // ===========================================================
    // Section 2: 最热页详细 Slot 分布
    // ===========================================================
    if (!sorted_pages.empty()) {
        const uint64_t   hottest_pid = sorted_pages[0].first;
        const PageStats& hottest_ps  = *sorted_pages[0].second;
        auto sv = sorted_slot_list(hottest_ps);

        out << "\n[Section 2: Detailed Slot Distribution of Hottest Page "
            << "(PageID=" << hottest_pid << ")]\n";
        out << "  Total Accesses : " << hottest_ps.total_accesses << "\n";
        out << "  Active Slots   : " << hottest_ps.slot_accesses.size()
            << " / " << slots_per_page << "  ("
            << std::fixed << std::setprecision(1)
            << 100.0 * hottest_ps.slot_accesses.size() / slots_per_page
            << "% of slots ever touched)\n\n";

        out << "  KEY INSIGHT: Page has " << hottest_ps.total_accesses
            << " accesses total. How concentrated are they?\n";
        out << std::string(72, '-') << "\n";
        out << std::left
            << std::setw(10) << "SlotRank"
            << std::setw(10) << "SlotID"
            << std::setw(12) << "Accesses"
            << std::setw(12) << "ThisSlot%"
            << "Cumulative%\n";
        out << std::string(72, '-') << "\n";

        uint64_t cumulative = 0;
        int print_limit = std::min((int)sv.size(), 15);
        for (int i = 0; i < print_limit; i++) {
            cumulative += sv[i].second;
            out << std::left
                << std::setw(10) << (i + 1)
                << std::setw(10) << sv[i].first
                << std::setw(12) << sv[i].second
                << std::setw(12) << std::fixed << std::setprecision(1)
                                 << 100.0 * sv[i].second / hottest_ps.total_accesses
                << std::fixed << std::setprecision(1)
                << 100.0 * cumulative / hottest_ps.total_accesses << "%\n";
        }
        if ((int)sv.size() > print_limit) {
            uint64_t rest = 0;
            for (int i = print_limit; i < (int)sv.size(); i++)
                rest += sv[i].second;
            cumulative += rest;
            out << "  ... other " << (sv.size() - print_limit)
                << " accessed slots: " << rest << " accesses ("
                << std::fixed << std::setprecision(1)
                << 100.0 * rest / hottest_ps.total_accesses << "%)\n";
        }
        uint64_t zero_slots = slots_per_page - (uint64_t)hottest_ps.slot_accesses.size();
        out << "  (+ " << zero_slots << " slots with 0 accesses)\n";

        // ASCII bar chart Top-10
        out << "\n  ASCII Bar Chart (Top-10 Slots):\n";
        uint64_t max_cnt = sv[0].second;
        int chart_w = 40;
        for (int i = 0; i < std::min((int)sv.size(), 10); i++) {
            int bar = (int)((double)sv[i].second / max_cnt * chart_w);
            out << "  Slot " << std::setw(4) << sv[i].first << " |"
                << std::string(bar, '#')
                << std::string(chart_w - bar, ' ')
                << "| " << sv[i].second << "\n";
        }

        // 关键结论
        double top5_pct = 100.0 * topK_sum(sv, 5) / hottest_ps.total_accesses;
        out << "\n  ==> Top-5 slots account for "
            << std::fixed << std::setprecision(1) << top5_pct
            << "% of this page's accesses.\n";
        out << "  ==> Promoting the entire page wastes "
            << std::fixed << std::setprecision(1) << (100.0 - top5_pct)
            << "% of DRAM/CXL bandwidth on cold records.\n";
    }

    // ===========================================================
    // Section 3: 批量统计 Top-100 热页的主导情况
    // ===========================================================
    out << "\n[Section 3: Domination Statistics Over Top-100 Hottest Pages]\n";
    out << "  Question: Among hot pages, how many are dominated by <=K records?\n\n";

    int analyzed  = 0;
    int dom_by_1  = 0, dom_by_3  = 0, dom_by_5  = 0, dom_by_10 = 0;
    double sum_r1 = 0, sum_r3   = 0, sum_r5   = 0;

    for (const auto& [pid, ps_ptr] : sorted_pages) {
        if (++analyzed > 100) break;
        const PageStats& ps = *ps_ptr;
        auto sv = sorted_slot_list(ps);

                double r1  = 100.0 * topK_sum(sv, 1)  / ps.total_accesses;
        double r3  = 100.0 * topK_sum(sv, 3)  / ps.total_accesses;
        double r5  = 100.0 * topK_sum(sv, 5)  / ps.total_accesses;
        double r10 = 100.0 * topK_sum(sv, 10) / ps.total_accesses;

        if (r1  >= 50.0) dom_by_1++;
        if (r3  >= 70.0) dom_by_3++;
        if (r5  >= 80.0) dom_by_5++;
        if (r10 >= 90.0) dom_by_10++;

        sum_r1 += r1;
        sum_r3 += r3;
        sum_r5 += r5;
    }

    int top_n = std::min(analyzed, 100);
    out << std::fixed << std::setprecision(1);
    out << "  Among top-" << top_n << " hottest pages:\n";
    out << std::string(60, '-') << "\n";
    out << std::left
        << std::setw(40) << "  Pages where Top-1 slot  >= 50% of accesses"
        << ": " << dom_by_1  << " / " << top_n
        << "  (" << 100.0 * dom_by_1  / top_n << "%)\n";
    out << std::left
        << std::setw(40) << "  Pages where Top-3 slots >= 70% of accesses"
        << ": " << dom_by_3  << " / " << top_n
        << "  (" << 100.0 * dom_by_3  / top_n << "%)\n";
    out << std::left
        << std::setw(40) << "  Pages where Top-5 slots >= 80% of accesses"
        << ": " << dom_by_5  << " / " << top_n
        << "  (" << 100.0 * dom_by_5  / top_n << "%)\n";
    out << std::left
        << std::setw(40) << "  Pages where Top-10 slots>= 90% of accesses"
        << ": " << dom_by_10 << " / " << top_n
        << "  (" << 100.0 * dom_by_10 / top_n << "%)\n";
    out << std::string(60, '-') << "\n";
    out << "  Avg Top-1  slot contribution : " << sum_r1 / top_n << "%\n";
    out << "  Avg Top-3  slot contribution : " << sum_r3 / top_n << "%\n";
    out << "  Avg Top-5  slot contribution : " << sum_r5 / top_n << "%\n";

    // ===========================================================
    // Section 4: 全局 Record 热度 vs Page 热度对比
    //            展示：热 Page 的热度来源于少数全局热 Record
    // ===========================================================
    out << "\n[Section 4: Record-Level vs Page-Level Access Distribution]\n";
    out << "  This section shows the access rank of hot records\n";
    out << "  that are assigned to the top-5 hottest pages.\n\n";

    // 重建 record_id -> access_count 的映射
    // 我们在 page_stats 里已有 slot 粒度数据，
    // 但没存 record_id。所以这里用文字说明即可，
    // 详细 record 级别分析见 Section 5（在主函数里额外统计）。
    out << "  (See Section 5 below for per-record access rank details.)\n";

    // ===========================================================
    // Section 5: Page 访问分布的 CDF
    //            展示：少数页面承担了大多数访问（Page-level Zipf）
    // ===========================================================
    out << "\n[Section 5: Page Access CDF — How Skewed is Page-Level Access?]\n";
    out << "  (What % of total accesses are covered by top-K% of pages?)\n\n";

    // 收集所有页面的访问次数
    std::vector<uint64_t> all_page_accesses;
    all_page_accesses.reserve(sorted_pages.size());
    for (const auto& [pid, ps_ptr] : sorted_pages)
        all_page_accesses.push_back(ps_ptr->total_accesses);
    // 已经是降序

    uint64_t grand_total = total_page_access_sum;
    uint64_t running     = 0;

    std::vector<double> cdf_pcts = {0.1, 0.5, 1.0, 5.0, 10.0, 20.0, 50.0};
    out << std::string(55, '-') << "\n";
    out << std::left
        << std::setw(20) << "  Top-X% of Pages"
        << std::setw(18) << "Page Count"
        << "% of Total Accesses\n";
    out << std::string(55, '-') << "\n";

    size_t cdf_idx = 0;
    running = 0;
    for (size_t i = 0; i < all_page_accesses.size() && cdf_idx < cdf_pcts.size(); i++) {
        running += all_page_accesses[i];
        double page_pct = 100.0 * (i + 1) / all_page_accesses.size();
        while (cdf_idx < cdf_pcts.size() && page_pct >= cdf_pcts[cdf_idx]) {
            out << std::left
                << std::setw(20) << ("  Top-" + std::to_string(cdf_pcts[cdf_idx]) + "%")
                << std::setw(18) << (i + 1)
                << std::fixed << std::setprecision(2)
                << 100.0 * running / grand_total << "%\n";
            cdf_idx++;
        }
    }
    out << std::string(55, '-') << "\n";

    // ===========================================================
    // Section 6: Waste Estimation
    //            如果按 Page 粒度迁移，有多少带宽是浪费的？
    // ===========================================================
    out << "\n[Section 6: Bandwidth Waste Estimation (Page vs Record Promotion)]\n";
    out << "  Assumption: promoting a page costs 16 KB regardless of\n";
    out << "  how many records are actually hot inside it.\n\n";

    double total_waste_pct = 0.0;
    int    waste_analyzed  = 0;

    for (const auto& [pid, ps_ptr] : sorted_pages) {
        if (++waste_analyzed > 100) break;
        const PageStats& ps = *ps_ptr;
        auto sv = sorted_slot_list(ps);
        // top-5 slots 认为是"真正热"的，其余是浪费
        double hot_pct  = 100.0 * topK_sum(sv, 5) / ps.total_accesses;
        double waste_pct = 100.0 - hot_pct;
        total_waste_pct += waste_pct;
    }

    double avg_waste = total_waste_pct / std::min(waste_analyzed, 100);
    out << std::fixed << std::setprecision(1);
    out << "  Among top-100 hottest pages:\n";
    out << "  - Avg % of page accesses that are 'cold' records : "
        << avg_waste << "%\n";
    out << "  - If we promote top-100 pages entirely            : "
        << "100 * 16KB = 1600 KB promoted\n";
    out << "  - Effectively wasted bandwidth                    : ~"
        << avg_waste / 100.0 * 1600.0 << " KB ("
        << avg_waste << "% of 1600 KB)\n";
    out << "  - Record-level promotion would save               : ~"
        << avg_waste / 100.0 * 1600.0 << " KB of DRAM/CXL capacity\n";

    out << "\n================================================================\n";
    out << " CONCLUSION\n";
    out << "================================================================\n";
    out << "  Under Zipfian(theta=" << zipf_theta << ") workload with hash-scattered\n";
    out << "  record placement:\n\n";
    out << "  1. Page accesses are highly skewed (few pages are hot).\n";
    out << "  2. Within each hot page, accesses concentrate on very few\n";
    out << "     slots (records), while most slots remain cold.\n";
    out << "  3. Promoting entire pages wastes ~" << avg_waste << "% of\n";
    out << "     DRAM/CXL bandwidth on cold records.\n";
    out << "  4. Record-granularity promotion is needed to efficiently\n";
    out << "     utilize limited DRAM/CXL capacity.\n";
    out << "================================================================\n";
}

// ============================================================
// 6. 主模拟函数
// ============================================================
void RunMotivationSimulation(double zipf_theta,
                              uint64_t total_requests,
                              const std::string& output_file)
{
    const uint16_t slots_per_page = 175;
    const uint64_t total_pages    = 262144;   // 4 GB / 16 KB
    const uint64_t total_records  = total_pages * slots_per_page; // ~45.8M

    std::cout << "=== Motivation Simulation ===\n";
    std::cout << "  total_pages   = " << total_pages   << "\n";
    std::cout << "  slots_per_page= " << slots_per_page << "\n";
    std::cout << "  total_records = " << total_records  << "\n";
    std::cout << "  zipf_theta    = " << zipf_theta     << "\n";
    std::cout << "  total_requests= " << total_requests  << "\n";
    std::cout << "  output_file   = " << output_file     << "\n\n";

    // --- 初始化 Zipf 生成器 ---
    std::cout << "Initializing Zipf generator... ";
    std::cout.flush();
    ZipfGenerator zipf(total_records, zipf_theta, /*seed=*/12345);
    std::cout << "done.\n";

    // --- 统计结构 ---
    // page_stats: page_id -> PageStats
    std::unordered_map<uint64_t, PageStats> page_stats;
    page_stats.reserve(total_pages);

    // record_access_count: record_id -> count（只记录被访问过的）
    // 用于 Section 5 的 record 热度分析
    std::unordered_map<uint64_t, uint64_t> record_access_count;
    record_access_count.reserve(1 << 20); // 预分配 1M

    // --- 主循环 ---
    std::cout << "Running " << total_requests << " requests...\n";
    auto t0 = std::chrono::high_resolution_clock::now();

    const uint64_t report_interval = total_requests / 10;
    for (uint64_t req = 0; req < total_requests; req++) {
        if (report_interval > 0 && req % report_interval == 0) {
            std::cout << "  Progress: " << req * 100 / total_requests << "%\r";
            std::cout.flush();
        }

        // 1. 从 Zipfian 分布中采样一个逻辑 record_id
        uint64_t record_id = zipf.next();

        // 2. 哈希打散：record_id → (page_id, slot_id)
        //    同一 record_id 永远映射到同一 (page, slot)
        RecordLocation loc = record_to_location(record_id, total_pages, slots_per_page);

        // 3. 更新统计
        auto& ps = page_stats[loc.page_id];
        ps.total_accesses++;
        ps.slot_accesses[loc.slot_id]++;

        record_access_count[record_id]++;
    }

    std::cout << "  Progress: 100%\n";
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "Simulation done in " << std::fixed << std::setprecision(2)
              << elapsed << "s\n";

    // --- 额外：record 热度排名（补充 Section 4）---
    std::vector<std::pair<uint64_t,uint64_t>> record_rank(
        record_access_count.begin(), record_access_count.end());
    std::sort(record_rank.begin(), record_rank.end(),
              [](const auto& a, const auto& b){ return a.second > b.second; });

    // --- 输出报告 ---
    std::cout << "Writing report to " << output_file << "...\n";
    std::ofstream fout(output_file);
    if (!fout) {
        std::cerr << "ERROR: cannot open " << output_file << "\n";
        return;
    }

    // 写入 header
    fout << "================================================================\n";
    fout << " Motivation Simulation Report\n";
    fout << " Generated: Zipf theta=" << zipf_theta
         << ", requests=" << total_requests << "\n";
    fout << "================================================================\n";

    // 主报告
    analyze_and_report(page_stats, slots_per_page,
                       total_requests, total_records,
                       zipf_theta, fout);

    // --- 补充 Section 4：Top-20 热 Record 的位置和访问次数 ---
    fout << "\n[Section 4 Detail: Top-20 Hottest Records]\n";
    fout << std::string(65, '-') << "\n";
    fout << std::left
         << std::setw(10) << "RecRank"
         << std::setw(16) << "RecordID"
         << std::setw(12) << "PageID"
         << std::setw(10) << "SlotID"
         << std::setw(12) << "Accesses"
         << "% of Total\n";
    fout << std::string(65, '-') << "\n";

    for (int i = 0; i < std::min((int)record_rank.size(), 20); i++) {
        uint64_t rid   = record_rank[i].first;
        uint64_t cnt   = record_rank[i].second;
        RecordLocation loc = record_to_location(rid, total_pages, slots_per_page);
        fout << std::left
             << std::setw(10) << (i + 1)
             << std::setw(16) << rid
             << std::setw(12) << loc.page_id
             << std::setw(10) << loc.slot_id
             << std::setw(12) << cnt
             << std::fixed << std::setprecision(4)
             << 100.0 * cnt / total_requests << "%\n";
    }
        fout << std::string(65, '-') << "\n";

    // --- 补充：验证"热页由热Record撑起"的直接证据 ---
    // 找出 Top-5 热页，展示其上的 record 在全局 record 热度排名中的位置
    fout << "\n[Section 4 Detail: For Each Top-5 Hot Page, Show Its Records' Global Rank]\n";
    fout << "  This directly proves: hot pages contain globally hot records.\n\n";

    // 建立 record_id -> global_rank 的反查表
    std::unordered_map<uint64_t, int> record_global_rank;
    record_global_rank.reserve(record_rank.size());
    for (int i = 0; i < (int)record_rank.size(); i++)
        record_global_rank[record_rank[i].first] = i + 1; // rank从1开始

    // 对每个页面，反推其上所有被访问的 record_id
    // 因为 record_to_location 是确定性哈希，
    // 我们需要从 record_access_count 中找出映射到该页的 record
    // 先建立 page_id -> [record_id] 的索引
    std::unordered_map<uint64_t, std::vector<uint64_t>> page_to_records;
    page_to_records.reserve(page_stats.size());
    for (const auto& [rid, cnt] : record_access_count) {
        RecordLocation loc = record_to_location(rid, total_pages, slots_per_page);
        page_to_records[loc.page_id].push_back(rid);
    }

    // 按总访问次数降序排列页面（复用之前的 sorted_pages）
    std::vector<std::pair<uint64_t, const PageStats*>> sorted_pages2;
    sorted_pages2.reserve(page_stats.size());
    for (const auto& [pid, ps] : page_stats)
        sorted_pages2.push_back({pid, &ps});
    std::sort(sorted_pages2.begin(), sorted_pages2.end(),
              [](const auto& a, const auto& b){
                  return a.second->total_accesses > b.second->total_accesses;
              });

    int page_rank = 0;
    for (const auto& [pid, ps_ptr] : sorted_pages2) {
        if (++page_rank > 5) break;
        const PageStats& ps = *ps_ptr;

        fout << "  Hot Page Rank #" << page_rank
             << "  (PageID=" << pid
             << ", TotalAccesses=" << ps.total_accesses << ")\n";
        fout << std::string(72, '-') << "\n";
        fout << std::left
             << std::setw(10) << "SlotID"
             << std::setw(16) << "RecordID"
             << std::setw(14) << "SlotAccesses"
             << std::setw(14) << "Slot%"
             << std::setw(14) << "GlobalRecRank"
             << "Category\n";
        fout << std::string(72, '-') << "\n";

        // 找出该页上的所有 record，按 slot 访问次数排序
        auto it = page_to_records.find(pid);
        if (it == page_to_records.end()) {
            fout << "  (no records found)\n\n";
            continue;
        }

        // 对该页上的 record 按访问次数降序排序
        std::vector<std::pair<uint64_t, uint64_t>> page_records; // (rid, cnt)
        for (uint64_t rid : it->second) {
            auto cnt_it = record_access_count.find(rid);
            if (cnt_it != record_access_count.end())
                page_records.push_back({rid, cnt_it->second});
        }
        std::sort(page_records.begin(), page_records.end(),
                  [](const auto& a, const auto& b){ return a.second > b.second; });

        for (int i = 0; i < (int)page_records.size(); i++) {
            uint64_t rid  = page_records[i].first;
            uint64_t cnt  = page_records[i].second;
            RecordLocation loc = record_to_location(rid, total_pages, slots_per_page);
            double slot_pct = 100.0 * cnt / ps.total_accesses;

            int g_rank = 0;
            auto gr_it = record_global_rank.find(rid);
            if (gr_it != record_global_rank.end()) g_rank = gr_it->second;

            std::string category;
            if      (g_rank <= 10)   category = "*** GLOBALLY TOP-10";
            else if (g_rank <= 100)  category = "**  GLOBALLY TOP-100";
            else if (g_rank <= 1000) category = "*   GLOBALLY TOP-1K";
            else                     category = "    cold";

            fout << std::left
                 << std::setw(10) << loc.slot_id
                 << std::setw(16) << rid
                 << std::setw(14) << cnt
                 << std::setw(14) << std::fixed << std::setprecision(1) << slot_pct
                 << std::setw(14) << g_rank
                 << category << "\n";
        }
        fout << "\n";
    }

    // ===========================================================
    // Section 7: Record Access 分布的 CDF
    //            展示：少数 record 承担了大多数访问
    // ===========================================================
    fout << "\n[Section 7: Record Access CDF — How Skewed is Record-Level Access?]\n";
    fout << "  (What % of total accesses are covered by top-K records?)\n\n";
    fout << std::string(55, '-') << "\n";
    fout << std::left
         << std::setw(20) << "  Top-K Records"
         << std::setw(18) << "Record Count"
         << "% of Total Accesses\n";
    fout << std::string(55, '-') << "\n";

    std::vector<int> record_top_ks = {1, 5, 10, 50, 100, 500, 1000, 5000, 10000};
    uint64_t running_rec = 0;
    int      rec_idx     = 0;
    for (int i = 0; i < (int)record_rank.size() && rec_idx < (int)record_top_ks.size(); i++) {
        running_rec += record_rank[i].second;
        while (rec_idx < (int)record_top_ks.size() && (i + 1) >= record_top_ks[rec_idx]) {
            fout << std::left
                 << std::setw(20) << ("  Top-" + std::to_string(record_top_ks[rec_idx]))
                 << std::setw(18) << record_top_ks[rec_idx]
                 << std::fixed << std::setprecision(4)
                 << 100.0 * running_rec / total_requests << "%\n";
            rec_idx++;
        }
    }
    fout << std::string(55, '-') << "\n";

    // ===========================================================
    // Section 8: 最终 Motivation 总结（直接可用于论文/报告）
    // ===========================================================
    fout << "\n[Section 8: Final Motivation Summary]\n";
    fout << std::string(65, '-') << "\n";

    // 计算几个关键数字
    uint64_t top1_rec_cnt  = record_rank.size() >= 1 ? record_rank[0].second : 0;
    uint64_t top5_rec_cnt  = 0;
    uint64_t top10_rec_cnt = 0;
    for (int i = 0; i < (int)record_rank.size() && i < 10; i++) {
        if (i < 5)  top5_rec_cnt  += record_rank[i].second;
        top10_rec_cnt += record_rank[i].second;
    }

    double top1_pct  = 100.0 * top1_rec_cnt  / total_requests;
    double top5_pct  = 100.0 * top5_rec_cnt  / total_requests;
    double top10_pct = 100.0 * top10_rec_cnt / total_requests;

    fout << std::fixed << std::setprecision(2);
    fout << "  Dataset Size     : 4 GB (" << total_pages << " pages x "
         << slots_per_page << " slots)\n";
    fout << "  Total Records    : " << total_records << "\n";
    fout << "  Total Requests   : " << total_requests << "\n";
    fout << "  Zipfian Theta    : " << zipf_theta << "\n\n";

    fout << "  [Record-Level Skew]\n";
    fout << "  - Top-1  record  covers : " << top1_pct  << "% of all accesses\n";
    fout << "  - Top-5  records cover  : " << top5_pct  << "% of all accesses\n";
    fout << "  - Top-10 records cover  : " << top10_pct << "% of all accesses\n\n";

    // 热页内的 skew 统计（取 top-20 热页的平均值）
    double avg_top1_slot_pct = 0.0;
    double avg_top3_slot_pct = 0.0;
    double avg_top5_slot_pct = 0.0;
    int    hot_page_count    = 0;

    for (const auto& [pid, ps_ptr] : sorted_pages2) {
        if (++hot_page_count > 20) break;
        const PageStats& ps = *ps_ptr;
        auto sv = sorted_slot_list(ps);
        avg_top1_slot_pct += 100.0 * topK_sum(sv, 1) / ps.total_accesses;
        avg_top3_slot_pct += 100.0 * topK_sum(sv, 3) / ps.total_accesses;
        avg_top5_slot_pct += 100.0 * topK_sum(sv, 5) / ps.total_accesses;
    }
    avg_top1_slot_pct /= hot_page_count;
    avg_top3_slot_pct /= hot_page_count;
    avg_top5_slot_pct /= hot_page_count;

    fout << "  [Page-Internal Skew — Avg over Top-20 Hot Pages]\n";
    fout << "  - Avg Top-1 slot  contributes : "
         << avg_top1_slot_pct << "% of page accesses\n";
    fout << "  - Avg Top-3 slots contribute  : "
         << avg_top3_slot_pct << "% of page accesses\n";
    fout << "  - Avg Top-5 slots contribute  : "
         << avg_top5_slot_pct << "% of page accesses\n\n";

    fout << "  [Key Takeaway]\n";
    fout << "  Under Zipfian(theta=" << zipf_theta << ") with hash-scattered placement,\n";
    fout << "  hot pages are driven by a tiny fraction of their records.\n";
    fout << "  On average, only top-5 slots account for "
         << avg_top5_slot_pct << "% of a hot page's accesses,\n";
    fout << "  yet page-granularity promotion would bring ALL "
         << slots_per_page << " records into DRAM/CXL.\n";
    fout << "  => Record-granularity admission control can save ~"
         << std::setprecision(1) << (100.0 - avg_top5_slot_pct)
         << "% of capacity\n";
    fout << "     that would otherwise be wasted on cold records.\n";
    fout << std::string(65, '-') << "\n";

    fout.close();
    std::cout << "Report written to " << output_file << "\n";
}

// ============================================================
// 7. main
// ============================================================
int main() {
    std::cout << "Starting Motivation Simulation...\n\n";

    // 参数配置
    const uint64_t total_requests = 30000000ULL; // 3000万请求

    // 不同 theta 值，观察 skew 程度对结论的影响
    std::vector<double> thetas = {0.90, 0.95, 0.99};

    for (double theta : thetas) {
        std::string fname = "motivation_theta_"
                          + std::to_string((int)(theta * 100))
                          + ".txt";
        std::cout << "\n--- Running theta=" << theta << " ---\n";
        RunMotivationSimulation(theta, total_requests, fname);
    }

    std::cout << "\nAll done. Check motivation_theta_*.txt files.\n";
    return 0;
}


