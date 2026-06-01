#include "backend/leanstore/storage/two-level-admission-control/DepulicateSetForPageSampling.hpp"
#include "backend/leanstore/storage/two-level-admission-control/CountMinSketch.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <unordered_set>
#include <algorithm>

using namespace leanstore::storage::two_level_admission_control;

// 测试辅助函数：打印测试分隔线
void PrintTestName(const std::string& test_name) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试: " << test_name << std::endl;
    std::cout << "========================================" << std::endl;
}

void PrintSuccess(const std::string& test_name) {
    std::cout << "[✓] " << test_name << " 通过!" << std::endl;
}

// ========== 测试 1: 基本功能测试 ==========
void TestBasicFunctionality() {
    PrintTestName("基本功能测试");
    
    // 创建去重采样集合，默认参数：1024 个 page_id，10000 次触发
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    
    // 验证初始状态
    assert(sampling_set.GetCurrentRequestCount() == 0);
    assert(sampling_set.GetSampledPageCount() == 0);
    assert(sampling_set.GetMaxSampledPageIds() == 1024);
    assert(sampling_set.GetTriggerVisitHistogramWindow() == 10000);
    assert(!sampling_set.ShouldTriggerVisitHistogramUpdate());
    
    std::cout << "初始状态验证通过:" << std::endl;
    std::cout << "  - 请求计数: " << sampling_set.GetCurrentRequestCount() << std::endl;
    std::cout << "  - 采样页面数: " << sampling_set.GetSampledPageCount() << std::endl;
    std::cout << "  - 最大采样数: " << sampling_set.GetMaxSampledPageIds() << std::endl;
    std::cout << "  - 触发窗口大小: " << sampling_set.GetTriggerVisitHistogramWindow() << std::endl;
    
    PrintSuccess("基本功能测试");
}

// ========== 测试 2: OnPageAccess 采样测试 ==========
void TestOnPageAccessSampling() {
    PrintTestName("OnPageAccess 采样测试");
    
    DepulicateSetForPageSampling sampling_set(10, 100);  // 小参数便于测试
    
    // 访问 5 个不同的页面
    for (u64 page_id = 100; page_id < 105; page_id++) {
        sampling_set.OnPageAccess(page_id);
    }
    
    assert(sampling_set.GetCurrentRequestCount() == 5);
    assert(sampling_set.GetSampledPageCount() == 5);
    
    std::cout << "访问 5 个不同页面后:" << std::endl;
    std::cout << "  - 请求计数: " << sampling_set.GetCurrentRequestCount() << std::endl;
    std::cout << "  - 采样页面数: " << sampling_set.GetSampledPageCount() << std::endl;
    
    // 重复访问同一个页面，采样集大小不变（去重）
    sampling_set.OnPageAccess(100);
    sampling_set.OnPageAccess(100);
    sampling_set.OnPageAccess(100);
    
    assert(sampling_set.GetCurrentRequestCount() == 8);  // 请求数继续增加
    assert(sampling_set.GetSampledPageCount() == 5);      // 采样数不变（去重）
    
    std::cout << "重复访问同一页面后:" << std::endl;
    std::cout << "  - 请求计数: " << sampling_set.GetCurrentRequestCount() << std::endl;
    std::cout << "  - 采样页面数: " << sampling_set.GetSampledPageCount() << " (去重生效)" << std::endl;
    
    PrintSuccess("OnPageAccess 采样测试");
}

// ========== 测试 3: 采样上限测试（1024 个 page_id）==========
void TestSamplingLimit() {
    PrintTestName("采样上限测试 (1024 个 page_id)");
    
    DepulicateSetForPageSampling sampling_set(1024, 50000);
    
    // 访问 1500 个不同的页面
    std::cout << "访问 1500 个不同页面..." << std::endl;
    for (u64 page_id = 1; page_id <= 1500; page_id++) {
        sampling_set.OnPageAccess(page_id);
    }
    
    u64 sampled_count = sampling_set.GetSampledPageCount();
    std::cout << "采样页面数: " << sampled_count << " (最多 1024)" << std::endl;
    
    // 由于使用 try_lock，可能采样数略多于 1024（并发竞争），但不应该远超
    assert(sampled_count <= 1100);  // 允许一定的超采样
    assert(sampling_set.GetCurrentRequestCount() == 1500);
    
    std::cout << "请求计数: " << sampling_set.GetCurrentRequestCount() << std::endl;
    
    PrintSuccess("采样上限测试");
}

// ========== 测试 4: 触发条件测试（1w 次请求）==========
void TestTriggerCondition() {
    PrintTestName("触发条件测试 (1w 次请求)");
    
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    
    // 访问 5000 次，不应触发
    std::cout << "访问 5000 次..." << std::endl;
    for (u64 i = 0; i < 5000; i++) {
        sampling_set.OnPageAccess(i % 100);  // 循环访问 100 个页面
    }
    
    assert(sampling_set.GetCurrentRequestCount() == 5000);
    assert(!sampling_set.ShouldTriggerVisitHistogramUpdate());
    std::cout << "5000 次访问后，未触发更新 ✓" << std::endl;
    
    // 继续访问到 10000 次，应该触发
    std::cout << "继续访问到 10000 次..." << std::endl;
    for (u64 i = 5000; i < 10000; i++) {
        sampling_set.OnPageAccess(i % 100);
    }
    
    assert(sampling_set.GetCurrentRequestCount() == 10000);
    assert(sampling_set.ShouldTriggerVisitHistogramUpdate());
    std::cout << "10000 次访问后，触发更新 ✓" << std::endl;
    
    // 超过 10000 次，依然应该触发
    sampling_set.OnPageAccess(999);
    assert(sampling_set.GetCurrentRequestCount() == 10001);
    assert(sampling_set.ShouldTriggerVisitHistogramUpdate());
    std::cout << "10001 次访问后，依然触发 ✓" << std::endl;
    
    PrintSuccess("触发条件测试");
}

// ========== 测试 5: 与 Count-Min-Sketch 集成测试 ==========
void TestCMSIntegration() {
    PrintTestName("Count-Min-Sketch 集成测试");
    
    // 创建 Page CMS 和采样集合
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(10, 100);
    
    // 模拟访问：page_id=100 访问 5 次，page_id=200 访问 3 次
    std::cout << "模拟页面访问：" << std::endl;
    for (int i = 0; i < 5; i++) {
        page_cms.CMSPageAccessUpdate(100);
        sampling_set.OnPageAccess(100);
    }
    for (int i = 0; i < 3; i++) {
        page_cms.CMSPageAccessUpdate(200);
        sampling_set.OnPageAccess(200);
    }
    
    std::cout << "  - Page 100: 访问 5 次" << std::endl;
    std::cout << "  - Page 200: 访问 3 次" << std::endl;
    
    // 填充采样集合的访问次数
    sampling_set.FillSampledPageVisitCounts(page_cms);
    
    // 获取采样数据
    auto sampled_data = sampling_set.GetSampledVisitCountSnapshot();
    
    std::cout << "采样数据填充后：" << std::endl;
    for (const auto& [page_id, visit_count] : sampled_data) {
        std::cout << "  - Page " << page_id << ": " << visit_count << " 次访问" << std::endl;
        
        if (page_id == 100) {
            assert(visit_count == 5);
        } else if (page_id == 200) {
            assert(visit_count == 3);
        }
    }
    
    assert(sampled_data.size() == 2);
    
    PrintSuccess("Count-Min-Sketch 集成测试");
}

// ========== 测试 6: Reset 重置测试 ==========
void TestReset() {
    PrintTestName("Reset 重置测试");
    
    DepulicateSetForPageSampling sampling_set(100, 1000);
    
    // 访问一些页面
    for (u64 page_id = 1; page_id <= 50; page_id++) {
        sampling_set.OnPageAccess(page_id);
    }
    
    std::cout << "Reset 前：" << std::endl;
    std::cout << "  - 请求计数: " << sampling_set.GetCurrentRequestCount() << std::endl;
    std::cout << "  - 采样页面数: " << sampling_set.GetSampledPageCount() << std::endl;
    
    assert(sampling_set.GetCurrentRequestCount() == 50);
    assert(sampling_set.GetSampledPageCount() > 0);
    
    // 执行重置
    sampling_set.Reset();
    
    std::cout << "Reset 后：" << std::endl;
    std::cout << "  - 请求计数: " << sampling_set.GetCurrentRequestCount() << std::endl;
    std::cout << "  - 采样页面数: " << sampling_set.GetSampledPageCount() << std::endl;
    
    assert(sampling_set.GetCurrentRequestCount() == 0);
    assert(sampling_set.GetSampledPageCount() == 0);
    assert(!sampling_set.ShouldTriggerVisitHistogramUpdate());
    
    PrintSuccess("Reset 重置测试");
}

// ========== 测试 7: 统计快照测试 ==========
void TestStatisticsSnapshot() {
    PrintTestName("统计快照测试");
    
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    
    // 访问 3000 次，采样 100 个不同页面
    for (u64 i = 0; i < 3000; i++) {
        sampling_set.OnPageAccess(i % 100);
    }
    
    auto stats = sampling_set.GetStatistics();
    
    std::cout << "统计快照信息：" << std::endl;
    std::cout << "  - 当前请求数: " << stats.current_request_count << std::endl;
    std::cout << "  - 采样页面数: " << stats.sampled_page_ids << std::endl;
    std::cout << "  - 最大采样数: " << stats.max_sampled_page_ids << std::endl;
    std::cout << "  - 触发窗口大小: " << stats.trigger_visit_histogram_update_size << std::endl;
    std::cout << "  - 采样进度: " << stats.sampling_progress * 100 << "%" << std::endl;
    std::cout << "  - 采样填充率: " << stats.sample_fill_ratio * 100 << "%" << std::endl;
    
    assert(stats.current_request_count == 3000);
    assert(stats.sampled_page_ids > 0);
    assert(stats.sampling_progress > 0.0 && stats.sampling_progress < 1.0);
    assert(stats.sample_fill_ratio >= 0.0 && stats.sample_fill_ratio <= 1.0);
    
    PrintSuccess("统计快照测试");
}

// ========== 测试 8: 多线程并发测试 ==========
void TestConcurrentAccess() {
    PrintTestName("多线程并发测试");
    
    DepulicateSetForPageSampling sampling_set(1024, 50000);
    
    const int num_threads = 8;
    const int iterations_per_thread = 1000;
    std::vector<std::thread> threads;
    
    std::cout << "启动 " << num_threads << " 个线程，每个访问 " 
              << iterations_per_thread << " 次..." << std::endl;
    
    // 每个线程访问不同范围的页面
    for (int tid = 0; tid < num_threads; tid++) {
        threads.emplace_back([&sampling_set, tid, iterations_per_thread]() {
            u64 page_id_start = tid * 100;
            for (int i = 0; i < iterations_per_thread; i++) {
                sampling_set.OnPageAccess(page_id_start + (i % 50));
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }
    
    u64 total_requests = sampling_set.GetCurrentRequestCount();
    u64 sampled_pages = sampling_set.GetSampledPageCount();
    
    std::cout << "并发访问完成：" << std::endl;
    std::cout << "  - 总请求数: " << total_requests << " (期望: " 
              << num_threads * iterations_per_thread << ")" << std::endl;
    std::cout << "  - 采样页面数: " << sampled_pages << std::endl;
    
    assert(total_requests == num_threads * iterations_per_thread);
    assert(sampled_pages > 0);
    assert(sampled_pages <= 1100);  // 允许少量超采样
    
    // 新增：验证 set 里没有重复的 page_id
    std::cout << "\n验证采样集合的正确性（无重复 page_id）..." << std::endl;
    auto sampled_data = sampling_set.GetSampledVisitCountSnapshot();
    
    // 使用 unordered_set 检查是否有重复
    std::unordered_set<u64> unique_page_ids;
    for (const auto& [page_id, visit_count] : sampled_data) {
        auto [iter, inserted] = unique_page_ids.insert(page_id);
        if (!inserted) {
            std::cerr << "错误：发现重复的 page_id: " << page_id << std::endl;
            assert(false);
        }
    }
    
    assert(unique_page_ids.size() == sampled_data.size());
    std::cout << "✓ 采样集合中无重复 page_id (验证了 " << unique_page_ids.size() << " 个唯一页面)" << std::endl;
    
    PrintSuccess("多线程并发测试");
}

// ========== 测试 9: 边界条件测试 ==========
void TestEdgeCases() {
    PrintTestName("边界条件测试");
    
    // 测试 1: 极小参数
    std::cout << "子测试 1: 极小参数 (max_sampled=1, trigger=1)" << std::endl;
    DepulicateSetForPageSampling small_set(1, 1);
    small_set.OnPageAccess(100);
    assert(small_set.GetCurrentRequestCount() == 1);
    assert(small_set.ShouldTriggerVisitHistogramUpdate());
    std::cout << "  ✓ 极小参数测试通过" << std::endl;
    
    // 测试 2: 单页重复访问
    std::cout << "\n子测试 2: 单页重复访问 1000 次" << std::endl;
    DepulicateSetForPageSampling single_page_set(100, 10000);
    for (int i = 0; i < 1000; i++) {
        single_page_set.OnPageAccess(999);
    }
    assert(single_page_set.GetCurrentRequestCount() == 1000);
    assert(single_page_set.GetSampledPageCount() == 1);
    std::cout << "  ✓ 单页重复访问测试通过" << std::endl;
    
    // 测试 3: 零除保护
    std::cout << "\n子测试 3: 零除保护测试" << std::endl;
    DepulicateSetForPageSampling zero_set(0, 0);
    auto stats = zero_set.GetStatistics();
    assert(stats.sampling_progress == 0.0);
    assert(stats.sample_fill_ratio == 0.0);
    std::cout << "  ✓ 零除保护测试通过" << std::endl;
    
    // 测试 4: 空集合调用 FillSampledPageVisitCounts
    std::cout << "\n子测试 4: 空集合填充测试" << std::endl;
    PageCountMinSketch cms(8, 1024);
    DepulicateSetForPageSampling empty_set(100, 1000);
    empty_set.FillSampledPageVisitCounts(cms);  // 不应崩溃
    auto data = empty_set.GetSampledVisitCountSnapshot();
    assert(data.empty());
    std::cout << "  ✓ 空集合填充测试通过" << std::endl;
    
    PrintSuccess("边界条件测试");
}

// ========== 测试 10: 完整工作流测试 ==========
void TestCompleteWorkflow() {
    PrintTestName("完整工作流测试");
    
    std::cout << "模拟一个完整的采样-更新-重置循环..." << std::endl;
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    
    // 第一轮：访问 10000 次
    std::cout << "\n--- 第一轮采样 (10000 次访问) ---" << std::endl;
    for (u64 i = 0; i < 10000; i++) {
        u64 page_id = i % 200;  // 200 个不同页面
        page_cms.CMSPageAccessUpdate(page_id);
        sampling_set.OnPageAccess(page_id);
    }
    
    assert(sampling_set.ShouldTriggerVisitHistogramUpdate());
    std::cout << "✓ 触发条件满足" << std::endl;
    
    // 填充访问次数
    sampling_set.FillSampledPageVisitCounts(page_cms);
    auto data_round1 = sampling_set.GetSampledVisitCountSnapshot();
    std::cout << "✓ 填充访问次数完成，采样 " << data_round1.size() << " 个页面" << std::endl;
    
    // 验证数据
    bool found_valid_data = false;
    for (const auto& [page_id, visit_count] : data_round1) {
        if (visit_count > 0) {
            found_valid_data = true;
            break;
        }
    }
    assert(found_valid_data);
    std::cout << "✓ 采样数据有效" << std::endl;
    
    // 重置准备下一轮
    sampling_set.Reset();
    assert(sampling_set.GetCurrentRequestCount() == 0);
    assert(sampling_set.GetSampledPageCount() == 0);
    std::cout << "✓ 重置完成，进入下一轮" << std::endl;
    
    // 第二轮：再访问 10000 次
    std::cout << "\n--- 第二轮采样 (10000 次访问) ---" << std::endl;
    for (u64 i = 0; i < 10000; i++) {
        u64 page_id = 500 + (i % 150);  // 访问不同范围的页面
        page_cms.CMSPageAccessUpdate(page_id);
        sampling_set.OnPageAccess(page_id);
    }
    
    assert(sampling_set.ShouldTriggerVisitHistogramUpdate());
    sampling_set.FillSampledPageVisitCounts(page_cms);
    auto data_round2 = sampling_set.GetSampledVisitCountSnapshot();
    std::cout << "✓ 第二轮采样完成，采样 " << data_round2.size() << " 个页面" << std::endl;
    
    std::cout << "\n完整工作流测试通过！" << std::endl;
    
    PrintSuccess("完整工作流测试");
}

// ========== 测试 11: 配置参数动态修改测试 ==========
void TestDynamicConfiguration() {
    PrintTestName("配置参数动态修改测试");
    
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    
    // 修改最大采样数
    std::cout << "修改最大采样数: 1024 -> 512" << std::endl;
    sampling_set.SetMaxSampledPageIds(512);
    assert(sampling_set.GetMaxSampledPageIds() == 512);
    
    // 修改触发窗口大小
    std::cout << "修改触发窗口: 10000 -> 5000" << std::endl;
    sampling_set.SetTriggerVisitHistogramWindow(5000);
    assert(sampling_set.GetTriggerVisitHistogramWindow() == 5000);
    
    // 验证新参数生效
    for (u64 i = 0; i < 5000; i++) {
        sampling_set.OnPageAccess(i);
    }
    
    assert(sampling_set.ShouldTriggerVisitHistogramUpdate());
    std::cout << "✓ 新的触发窗口 (5000) 生效" << std::endl;
    
    PrintSuccess("配置参数动态修改测试");
}

// ========== 测试 12: Fill -> Reset 顺序测试 ==========
void TestFillResetSequence() {
    PrintTestName("Fill -> Reset 顺序测试");
    
    std::cout << "测试正确的工作流: Fill -> Reset -> 下一轮采样" << std::endl;
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(100, 1000);
    
    // 第一轮采样
    std::cout << "\n--- 第一轮采样 ---" << std::endl;
    for (u64 i = 0; i < 50; i++) {
        page_cms.CMSPageAccessUpdate(i);
        sampling_set.OnPageAccess(i);
    }
    
    std::cout << "第一轮采样: " << sampling_set.GetSampledPageCount() << " 个页面" << std::endl;
    
    // Fill 访问次数
    sampling_set.FillSampledPageVisitCounts(page_cms);
    auto data_before_reset = sampling_set.GetSampledVisitCountSnapshot();
    std::cout << "Fill 完成，数据条数: " << data_before_reset.size() << std::endl;
    
    // 验证 Fill 后数据有效
    bool has_valid_count = false;
    for (const auto& [page_id, visit_count] : data_before_reset) {
        if (visit_count > 0) {
            has_valid_count = true;
            std::cout << "  - Page " << page_id << ": " << visit_count << " 次" << std::endl;
        }
    }
    assert(has_valid_count);
    std::cout << "✓ Fill 后数据有效" << std::endl;
    
    // Reset
    std::cout << "\n执行 Reset..." << std::endl;
    sampling_set.Reset();
    assert(sampling_set.GetCurrentRequestCount() == 0);
    assert(sampling_set.GetSampledPageCount() == 0);
    std::cout << "✓ Reset 后计数器清零" << std::endl;
    
    // 第二轮采样
    std::cout << "\n--- 第二轮采样 ---" << std::endl;
    for (u64 i = 100; i < 150; i++) {
        page_cms.CMSPageAccessUpdate(i);
        sampling_set.OnPageAccess(i);
    }
    
    std::cout << "第二轮采样: " << sampling_set.GetSampledPageCount() << " 个页面" << std::endl;
    
    sampling_set.FillSampledPageVisitCounts(page_cms);
    auto data_after_reset = sampling_set.GetSampledVisitCountSnapshot();
    
    // 验证第二轮数据与第一轮不同
    std::cout << "验证第二轮采样的页面与第一轮不同..." << std::endl;
    for (const auto& [page_id, visit_count] : data_after_reset) {
        assert(page_id >= 100 && page_id < 150);  // 第二轮的页面范围
        std::cout << "  - Page " << page_id << ": " << visit_count << " 次" << std::endl;
    }
    std::cout << "✓ 第二轮采样独立且正确" << std::endl;
    
    PrintSuccess("Fill -> Reset 顺序测试");
}

// ========== 测试 13: Fill -> Fill（不 Reset）覆盖测试 ==========
void TestDoubleFillWithoutReset() {
    PrintTestName("Fill -> Fill (不 Reset) 覆盖测试");
    
    std::cout << "测试场景: Fill 之后不 Reset 直接再 Fill 会怎样？" << std::endl;
    std::cout << "预期行为: visit_count 会被覆盖为新值（而不是累加）\n" << std::endl;
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(100, 10000);
    
    // 采样几个页面
    std::cout << "--- 初始采样阶段 ---" << std::endl;
    u64 test_page_id = 42;
    
    // Page 42 访问 5 次
    for (int i = 0; i < 5; i++) {
        page_cms.CMSPageAccessUpdate(test_page_id);
        sampling_set.OnPageAccess(test_page_id);
    }
    
    // Page 100 访问 3 次
    for (int i = 0; i < 3; i++) {
        page_cms.CMSPageAccessUpdate(100);
        sampling_set.OnPageAccess(100);
    }
    
    std::cout << "CMS 中 Page 42 访问次数: " << page_cms.CMSGetPageAccessCount(test_page_id) << std::endl;
    std::cout << "CMS 中 Page 100 访问次数: " << page_cms.CMSGetPageAccessCount(100) << std::endl;
    
    // 第一次 Fill
    std::cout << "\n--- 第一次 Fill ---" << std::endl;
    sampling_set.FillSampledPageVisitCounts(page_cms);
    auto data_first_fill = sampling_set.GetSampledVisitCountSnapshot();
    
    u64 page42_count_first_fill = 0;
    u64 page100_count_first_fill = 0;
    for (const auto& [page_id, visit_count] : data_first_fill) {
        std::cout << "Page " << page_id << ": " << visit_count << " 次" << std::endl;
        if (page_id == test_page_id) {
            page42_count_first_fill = visit_count;
        }
        if (page_id == 100) {
            page100_count_first_fill = visit_count;
        }
    }
    
    assert(page42_count_first_fill == 5);
    assert(page100_count_first_fill == 3);
    std::cout << "✓ 第一次 Fill: Page 42 = " << page42_count_first_fill 
              << ", Page 100 = " << page100_count_first_fill << std::endl;
    
    // CMS 继续累积访问（不清空）
    std::cout << "\n--- CMS 继续累积访问 ---" << std::endl;
    // Page 42 再访问 10 次
    for (int i = 0; i < 10; i++) {
        page_cms.CMSPageAccessUpdate(test_page_id);
    }
    // Page 100 再访问 7 次
    for (int i = 0; i < 7; i++) {
        page_cms.CMSPageAccessUpdate(100);
    }
    
    std::cout << "CMS 中 Page 42 现在访问次数: " << page_cms.CMSGetPageAccessCount(test_page_id) 
              << " (之前 5，新增 10，总共 15)" << std::endl;
    std::cout << "CMS 中 Page 100 现在访问次数: " << page_cms.CMSGetPageAccessCount(100) 
              << " (之前 3，新增 7，总共 10)" << std::endl;
    
    // 第二次 Fill（不 Reset）
    std::cout << "\n--- 第二次 Fill (不 Reset) ---" << std::endl;
    sampling_set.FillSampledPageVisitCounts(page_cms);
    auto data_second_fill = sampling_set.GetSampledVisitCountSnapshot();
    
    u64 page42_count_second_fill = 0;
    u64 page100_count_second_fill = 0;
    for (const auto& [page_id, visit_count] : data_second_fill) {
        std::cout << "Page " << page_id << ": " << visit_count << " 次" << std::endl;
        if (page_id == test_page_id) {
            page42_count_second_fill = visit_count;
        }
        if (page_id == 100) {
            page100_count_second_fill = visit_count;
        }
    }
    
    // 关键验证：visit_count 应该是 CMS 的当前值（新值），而不是累加
    std::cout << "\n--- 关键验证 ---" << std::endl;
    std::cout << "Page 42: 第一次 Fill = " << page42_count_first_fill 
              << ", 第二次 Fill = " << page42_count_second_fill 
              << " (预期: 15, 覆盖为新值)" << std::endl;
    std::cout << "Page 100: 第一次 Fill = " << page100_count_first_fill 
              << ", 第二次 Fill = " << page100_count_second_fill 
              << " (预期: 10, 覆盖为新值)" << std::endl;
    
    // 验证是覆盖（新值），而不是累加（5+15=20 或 3+10=13）
    assert(page42_count_second_fill == 15);  // 新值，不是 5+15=20
    assert(page100_count_second_fill == 10); // 新值，不是 3+10=13
    
    std::cout << "✓ 验证通过: visit_count 是覆盖为新值，而不是累加" << std::endl;
    
    PrintSuccess("Fill -> Fill (不 Reset) 覆盖测试");
}

// ========== 主测试函数 ==========
int main() {
    std::cout << "\n" << std::endl;
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     DepulicateSetForPageSampling 组件测试套件             ║" << std::endl;
    std::cout << "║     Two-Level Admission Control System                    ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;
    
    try {
        // 运行所有测试
        TestBasicFunctionality();           // 1. 基本功能
        TestOnPageAccessSampling();         // 2. 采样功能
        TestSamplingLimit();                // 3. 采样上限
        TestTriggerCondition();             // 4. 触发条件
        TestCMSIntegration();               // 5. CMS 集成
        TestReset();                        // 6. 重置功能
        TestStatisticsSnapshot();           // 7. 统计快照
        TestConcurrentAccess();             // 8. 并发测试（含去重验证）
        TestEdgeCases();                    // 9. 边界条件
        TestCompleteWorkflow();             // 10. 完整工作流
        TestDynamicConfiguration();         // 11. 动态配置
        TestFillResetSequence();            // 12. Fill -> Reset 顺序测试
        TestDoubleFillWithoutReset();       // 13. Fill -> Fill (不 Reset) 覆盖测试
        
        // 所有测试通过
        std::cout << "\n" << std::endl;
        std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║                  所有测试通过! ✓                           ║" << std::endl;
        std::cout << "║                  13 个测试全部成功                         ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;
        std::cout << "\n" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[✗] 测试失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n[✗] 测试失败: 未知异常" << std::endl;
        return 1;
    }
}
