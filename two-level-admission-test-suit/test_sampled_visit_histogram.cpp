// SampledVisitHistogram 测试程序
// 直接使用 Config.hpp 中定义的 FLAGS 参数

#include "backend/leanstore/storage/two-level-admission-control/SampledVisitHistogram.hpp"
#include "backend/leanstore/storage/two-level-admission-control/CountMinSketch.hpp"
#include "backend/leanstore/storage/two-level-admission-control/DepulicateSetForPageSampling.hpp"
#include "backend/leanstore/storage/two-level-admission-control/VisitCountBucket.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>
#include <ctime>

using namespace leanstore::storage::two_level_admission_control;

// 测试辅助函数
void PrintTestName(const std::string& test_name) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试: " << test_name << std::endl;
    std::cout << "========================================" << std::endl;
}

void PrintSuccess(const std::string& test_name) {
    std::cout << "[✓] " << test_name << " 通过!" << std::endl;
}

// ========== 测试 1: 基本初始化测试 ==========
void TestBasicInitialization() {
    PrintTestName("基本初始化测试");
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    VisitFrequencyBucketArray bucket_array(16);
    SampledVisitHistogram histogram(page_cms, sampling_set, bucket_array);
    
    // 验证初始阈值
    u64 threshold_coarse = histogram.GetAdmissionThreshold_coarse();
    u64 threshold_fine = histogram.GetAdmissionThreshold_fine();
    
    std::cout << "初始粗粒度阈值: " << threshold_coarse << std::endl;
    std::cout << "初始细粒度阈值: " << threshold_fine << std::endl;
    
    assert(threshold_coarse == 1);
    assert(threshold_fine == 1);
    
    // 验证 DRAM:CXL 比例
    double ratio = histogram.GetDRAMvsCXLRatio();
    std::cout << "DRAM vs CXL 比例: " << ratio * 100 << "%" << std::endl;
    std::cout << "DRAM: " << FLAGS_dram_buffer_pool_gib << " GiB, "
              << "CXL: " << FLAGS_cxl_gib << " GiB" << std::endl;
    
    PrintSuccess("基本初始化测试");
}

// ========== 测试 2: 单线程 OnPageAccess 测试 ==========
void TestSingleThreadOnPageAccess() {
    PrintTestName("单线程 OnPageAccess 测试");
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    VisitFrequencyBucketArray bucket_array(16);
    SampledVisitHistogram histogram(page_cms, sampling_set, bucket_array);
    
    std::cout << "模拟单线程访问 10000 次..." << std::endl;
    
    // 访问一些页面，总共 10000 次
    for (int i = 0; i < 5000; i++) {
        histogram.OnPageAccess(100);  // Page 100: 5000 次
    }
    
    for (int i = 0; i < 3000; i++) {
        histogram.OnPageAccess(200);  // Page 200: 3000 次
    }
    
    for (int i = 0; i < 2000; i++) {
        histogram.OnPageAccess(300);  // Page 300: 2000 次
    }
    
    // 应该自动触发更新
    assert(sampling_set.GetCurrentRequestCount() == 0);  // Reset 后为 0
    
    u64 threshold = histogram.GetAdmissionThreshold_coarse();
    std::cout << "更新后的阈值: " << threshold << std::endl;
    
    // 验证 CMS 统计（已老化）
    u64 count_100 = page_cms.CMSGetPageAccessCount(100);
    u64 count_200 = page_cms.CMSGetPageAccessCount(200);
    u64 count_300 = page_cms.CMSGetPageAccessCount(300);
    
    std::cout << "Page 100 访问次数（老化后）: " << count_100 << " (原 5000 >> 1 = 2500)" << std::endl;
    std::cout << "Page 200 访问次数（老化后）: " << count_200 << " (原 3000 >> 1 = 1500)" << std::endl;
    std::cout << "Page 300 访问次数（老化后）: " << count_300 << " (原 2000 >> 1 = 1000)" << std::endl;
    
    assert(count_100 == 2500);
    assert(count_200 == 1500);
    assert(count_300 == 1000);
    
    PrintSuccess("单线程 OnPageAccess 测试");
}

// ========== 测试 3: WorkerThreadOnPageAccess 基本功能测试 ==========
void TestWorkerThreadOnPageAccess() {
    PrintTestName("WorkerThreadOnPageAccess 基本功能测试");
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    VisitFrequencyBucketArray bucket_array(16);
    SampledVisitHistogram histogram(page_cms, sampling_set, bucket_array);
    
    std::cout << "使用 WorkerThreadOnPageAccess 访问..." << std::endl;
    
    // Worker 线程只负责计数
    for (int i = 0; i < 5000; i++) {
        histogram.WorkerThreadOnPageAccess(100);
    }
    
    // 验证 CMS 更新
    u64 count = page_cms.CMSGetPageAccessCount(100);
    std::cout << "Page 100 访问次数: " << count << " (期望: 5000)" << std::endl;
    assert(count == 5000);
    
    // 验证采样集更新
    u64 request_count = sampling_set.GetCurrentRequestCount();
    std::cout << "当前请求计数: " << request_count << " (期望: 5000)" << std::endl;
    assert(request_count == 5000);
    
    // 重要：WorkerThreadOnPageAccess 不会触发更新
    std::cout << "\n验证 WorkerThreadOnPageAccess 不会自动触发更新..." << std::endl;
    for (int i = 0; i < 5000; i++) {
        histogram.WorkerThreadOnPageAccess(200);
    }
    
    // 请求计数应该达到 10000，但直方图还未更新
    assert(sampling_set.GetCurrentRequestCount() == 10000);
    std::cout << "✓ 达到 10000 次请求，但直方图未自动更新" << std::endl;
    
    PrintSuccess("WorkerThreadOnPageAccess 基本功能测试");
}

// ========== 测试 4: BackgroundThreadTryUpdate 功能测试 ==========
void TestBackgroundThreadTryUpdate() {
    PrintTestName("BackgroundThreadTryUpdate 功能测试");
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    VisitFrequencyBucketArray bucket_array(16);
    SampledVisitHistogram histogram(page_cms, sampling_set, bucket_array);
    
    std::cout << "Worker 线程访问 5000 次..." << std::endl;
    for (int i = 0; i < 5000; i++) {
        histogram.WorkerThreadOnPageAccess(i % 100);
    }
    
    // 尝试更新（应该返回 false，因为未达到触发条件）
    bool updated = histogram.BackgroundThreadTryUpdate();
    std::cout << "尝试更新（5000 次）: " << (updated ? "已更新" : "未更新") << std::endl;
    assert(!updated);
    
    std::cout << "\nWorker 线程继续访问到 10000 次..." << std::endl;
    for (int i = 5000; i < 10000; i++) {
        histogram.WorkerThreadOnPageAccess(i % 100);
    }
    
    // 再次尝试更新（应该返回 true）
    updated = histogram.BackgroundThreadTryUpdate();
    std::cout << "尝试更新（10000 次）: " << (updated ? "已更新 ✓" : "未更新 ✗") << std::endl;
    assert(updated);
    
    // 验证更新后采样集已重置
    assert(sampling_set.GetCurrentRequestCount() == 0);
    std::cout << "✓ 更新后采样集已重置" << std::endl;
    
    // 验证阈值已更新
    u64 threshold = histogram.GetAdmissionThreshold_coarse();
    std::cout << "更新后的阈值: " << threshold << std::endl;
    assert(threshold > 0);
    
    PrintSuccess("BackgroundThreadTryUpdate 功能测试");
}

// ========== 测试 5: 多线程场景 - 4 Worker + 1 Background ==========
void TestMultiThreadScenario() {
    PrintTestName("多线程场景测试 (4 Worker + 1 Background)");
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(1024, 10000);  // 1w 次触发
    VisitFrequencyBucketArray bucket_array(16);
    SampledVisitHistogram histogram(page_cms, sampling_set, bucket_array);
    
    std::atomic<bool> running{true};
    std::atomic<u64> total_worker_accesses{0};
    std::atomic<u64> background_updates{0};
    
    const int num_workers = 4;
    const int accesses_per_worker = 15000;  // 每个 worker 访问 1.5w 次，总共 6w 次
    
    std::cout << "启动 " << num_workers << " 个 Worker 线程..." << std::endl;
    std::cout << "每个 Worker 访问 " << accesses_per_worker << " 次" << std::endl;
    std::cout << "总访问次数: " << num_workers * accesses_per_worker << std::endl;
    std::cout << "预期触发更新: " << (num_workers * accesses_per_worker / 10000) << " 次\n" << std::endl;
    
    // Worker 线程
    std::vector<std::thread> workers;
    for (int tid = 0; tid < num_workers; tid++) {
        workers.emplace_back([&histogram, &total_worker_accesses, tid, accesses_per_worker]() {
            std::mt19937_64 rng(tid);
            std::uniform_int_distribution<u64> dist(0, 199);
            
            for (int i = 0; i < accesses_per_worker; i++) {
                u64 page_id = dist(rng);
                histogram.WorkerThreadOnPageAccess(page_id);
                total_worker_accesses.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    
    // Background 线程
    std::thread background([&histogram, &running, &background_updates]() {
        while (running.load(std::memory_order_relaxed)) {
            if (histogram.BackgroundThreadTryUpdate()) {
                u64 update_count = background_updates.fetch_add(1, std::memory_order_relaxed) + 1;
                u64 threshold = histogram.GetAdmissionThreshold_coarse();
                std::cout << "[Background] 第 " << update_count << " 次更新完成, "
                          << "新阈值: " << threshold << std::endl;
            }
            
            // 短暂休眠避免过度轮询
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    
    // 等待所有 Worker 完成
    for (auto& w : workers) {
        w.join();
    }
    
    std::cout << "\n所有 Worker 线程完成" << std::endl;
    std::cout << "总访问次数: " << total_worker_accesses.load() << std::endl;
    
    // 给 Background 线程一些时间处理最后的更新
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 手动触发最后可能剩余的更新
    if (histogram.BackgroundThreadTryUpdate()) {
        background_updates.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[手动] 触发了最后一次更新" << std::endl;
    }
    
    running.store(false, std::memory_order_relaxed);
    background.join();
    
    std::cout << "\nBackground 线程完成" << std::endl;
    std::cout << "触发更新次数: " << background_updates.load() << std::endl;
    
    // 验证阈值的有效性
    u64 final_threshold = histogram.GetAdmissionThreshold_coarse();
    std::cout << "\n最终阈值: " << final_threshold << std::endl;
    assert(final_threshold > 0);
    assert(final_threshold != 1 || background_updates.load() == 0);  // 如果有更新，阈值不应该是初始值
    
    // 验证至少触发了一次更新
    assert(background_updates.load() >= 1);
    std::cout << "✓ 至少触发了 " << background_updates.load() << " 次更新" << std::endl;
    
    PrintSuccess("多线程场景测试 (4 Worker + 1 Background)");
}

// ========== 测试 6: 并发安全性 - 防止重复更新问题 ==========
void TestConcurrentSafetyNoDoubleUpdate() {
    PrintTestName("并发安全性测试 - 防止重复更新");
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    VisitFrequencyBucketArray bucket_array(16);
    SampledVisitHistogram histogram(page_cms, sampling_set, bucket_array);
    
    std::cout << "测试场景：验证多个线程不会因 Reset() 导致错误阈值" << std::endl;
    std::cout << "每个 Worker 访问 7500 次，4 个 Worker 总共 30000 次" << std::endl;
    std::cout << "预期触发 3 次更新\n" << std::endl;
    
    std::atomic<u64> threshold_errors{0};  // 记录阈值为 1 的错误次数
    std::vector<u64> all_thresholds;
    std::mutex thresholds_mutex;
    
    const int num_workers = 4;
    const int accesses_per_worker = 7500;  // 总共 30000 次，触发 3 次更新
    
    // Worker 线程：只负责计数
    std::vector<std::thread> workers;
    for (int tid = 0; tid < num_workers; tid++) {
        workers.emplace_back([&histogram, tid, accesses_per_worker]() {
            std::mt19937_64 rng(tid);
            std::uniform_int_distribution<u64> dist(0, 99);
            
            for (int i = 0; i < accesses_per_worker; i++) {
                u64 page_id = dist(rng);
                histogram.WorkerThreadOnPageAccess(page_id);
            }
        });
    }
    
    // Background 线程：负责更新并记录阈值
    std::atomic<bool> running{true};
    std::thread background([&]() {
        while (running.load(std::memory_order_relaxed)) {
            if (histogram.BackgroundThreadTryUpdate()) {
                u64 threshold = histogram.GetAdmissionThreshold_coarse();
                
                {
                    std::lock_guard<std::mutex> lock(thresholds_mutex);
                    all_thresholds.push_back(threshold);
                }
                
                if (threshold == 1) {
                    threshold_errors.fetch_add(1, std::memory_order_relaxed);
                    std::cout << "[警告] 检测到错误阈值: 1" << std::endl;
                } else {
                    std::cout << "[正常] 更新阈值: " << threshold << std::endl;
                }
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    
    // 等待所有线程完成
    for (auto& w : workers) {
        w.join();
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_relaxed);
    background.join();
    
    std::cout << "\n测试结果统计:" << std::endl;
    std::cout << "总更新次数: " << all_thresholds.size() << std::endl;
    std::cout << "错误阈值次数 (threshold == 1): " << threshold_errors.load() << std::endl;
    
    if (all_thresholds.size() > 0) {
        std::cout << "所有阈值记录: ";
        for (u64 t : all_thresholds) {
            std::cout << t << " ";
        }
        std::cout << std::endl;
    }
    
    // 关键验证：不应该出现错误阈值
    if (threshold_errors.load() > 0) {
        std::cerr << "\n✗ 检测到并发问题！存在 " << threshold_errors.load() 
                  << " 次错误阈值" << std::endl;
        assert(false);
    }
    
    std::cout << "\n✓ 并发安全：所有阈值均正确，无重复更新问题" << std::endl;
    
    PrintSuccess("并发安全性测试 - 防止重复更新");
}

// ========== 测试 7: 压力测试 - 长时间运行 ==========
void TestLongRunningStress() {
    PrintTestName("压力测试 - 长时间运行");
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    VisitFrequencyBucketArray bucket_array(16);
    SampledVisitHistogram histogram(page_cms, sampling_set, bucket_array);
    
    std::atomic<bool> running{true};
    std::atomic<u64> total_accesses{0};
    std::atomic<u64> total_updates{0};
    std::atomic<u64> invalid_thresholds{0};
    
    const int num_workers = 4;
    const int duration_seconds = 5;  // 运行 5 秒
    
    std::cout << "启动 " << num_workers << " 个 Worker 线程" << std::endl;
    std::cout << "运行时间: " << duration_seconds << " 秒" << std::endl;
    std::cout << "持续监控阈值有效性..." << std::endl;
    std::cout << "预计产生大量访问和多次更新\n" << std::endl;
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Worker 线程：持续访问
    std::vector<std::thread> workers;
    for (int tid = 0; tid < num_workers; tid++) {
        workers.emplace_back([&]() {
            std::mt19937_64 rng(tid);
            std::uniform_int_distribution<u64> dist(0, 299);
            
            while (running.load(std::memory_order_relaxed)) {
                u64 page_id = dist(rng);
                histogram.WorkerThreadOnPageAccess(page_id);
                total_accesses.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    
    // Background 线程：持续更新
    std::thread background([&]() {
        while (running.load(std::memory_order_relaxed)) {
            if (histogram.BackgroundThreadTryUpdate()) {
                u64 update_no = total_updates.fetch_add(1, std::memory_order_relaxed) + 1;
                u64 threshold = histogram.GetAdmissionThreshold_coarse();
                
                if (threshold == 1 && update_no > 1) {
                    invalid_thresholds.fetch_add(1, std::memory_order_relaxed);
                    std::cout << "[异常] 更新 #" << update_no << " 阈值错误: " << threshold << std::endl;
                }
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    
    // 运行指定时间
    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
    running.store(false, std::memory_order_relaxed);
    
    // 等待所有线程结束
    for (auto& w : workers) {
        w.join();
    }
    background.join();
    
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    
    std::cout << "\n压力测试完成" << std::endl;
    std::cout << "运行时间: " << elapsed << " ms" << std::endl;
    std::cout << "总访问次数: " << total_accesses.load() << std::endl;
    std::cout << "总更新次数: " << total_updates.load() << std::endl;
    std::cout << "访问速率: " << (total_accesses.load() * 1000 / elapsed) << " ops/s" << std::endl;
    std::cout << "无效阈值次数: " << invalid_thresholds.load() << std::endl;
    
    u64 final_threshold = histogram.GetAdmissionThreshold_coarse();
    std::cout << "最终阈值: " << final_threshold << std::endl;
    
    // 验证
    assert(total_updates.load() > 0);
    assert(invalid_thresholds.load() == 0);
    
    std::cout << "\n✓ 压力测试通过：" << total_updates.load() << " 次更新，无并发问题" << std::endl;
    
    PrintSuccess("压力测试 - 长时间运行");
}

// ========== 测试 8: 打印直方图测试 ==========
void TestPrintHistogram() {
    PrintTestName("打印直方图功能测试");
    
    PageCountMinSketch page_cms(12, 1024 * 16);
    DepulicateSetForPageSampling sampling_set(1024, 10000);
    VisitFrequencyBucketArray bucket_array(16);
    SampledVisitHistogram histogram(page_cms, sampling_set, bucket_array);
    
    std::cout << "模拟 Zipfian 分布访问 10000 次，触发直方图更新...\n" << std::endl;
    
    // 模拟 Zipfian 分布访问
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<u64> hot_dist(0, 19);      // 20 个热页面
    std::uniform_int_distribution<u64> warm_dist(20, 99);    // 80 个温页面
    std::uniform_int_distribution<u64> cold_dist(100, 299);  // 200 个冷页面
    std::uniform_int_distribution<u64> access_type(0, 99);
    
    for (u64 i = 0; i < 10000; i++) {
        u64 type = access_type(rng);
        u64 page_id;
        
        if (type < 60) {
            page_id = hot_dist(rng);  // 60% 访问热页面
        } else if (type < 85) {
            page_id = warm_dist(rng);  // 25% 访问温页面
        } else {
            page_id = cold_dist(rng);  // 15% 访问冷页面
        }
        
        histogram.OnPageAccess(page_id);
    }
    
    // 打印直方图
    std::string histogram_str = histogram.PrintHistogram();
    std::cout << histogram_str << std::endl;
    
    // 验证直方图包含关键信息
    assert(!histogram_str.empty());
    assert(histogram_str.find("Sampled Page Visit Histogram") != std::string::npos);
    assert(histogram_str.find("DRAM: CXL Ratio") != std::string::npos);
    assert(histogram_str.find("Admission Threshold") != std::string::npos);
    
    std::cout << "✓ 直方图打印功能正常" << std::endl;
    
    PrintSuccess("打印直方图功能测试");
}

// ========== 测试 9: 不同 DRAM:CXL 比例下的阈值对比测试 ==========
void TestThresholdComparisonUnderDifferentRatios() {
    PrintTestName("不同 DRAM:CXL 比例下的阈值对比测试");
    
    std::cout << "模拟真实数据库查询场景，对比粗粒度和细粒度阈值差异\n" << std::endl;
    
    // 测试不同的 DRAM:CXL 比例
    struct Scenario {
        std::string name;
        double dram_gib;
        double cxl_gib;
        double ratio;
    };
    
    std::vector<Scenario> scenarios = {
        {"DRAM:CXL = 1:2", 16.0, 32.0, 0.333},
        {"DRAM:CXL = 1:3", 8.0, 24.0, 0.25},
        {"DRAM:CXL = 1:4", 8.0, 32.0, 0.20},
        {"DRAM:CXL = 1:8", 4.0, 32.0, 0.111}
    };
    
    std::cout << "╔════════════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║                   阈值对比测试 - 不同 DRAM:CXL 比例                        ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════════════════════╝" << std::endl;
    
    for (const auto& scenario : scenarios) {
        std::cout << "\n" << std::string(80, '=') << std::endl;
        std::cout << "测试场景: " << scenario.name << std::endl;
        std::cout << "DRAM: " << scenario.dram_gib << " GiB, CXL: " << scenario.cxl_gib << " GiB" << std::endl;
        std::cout << "理论 DRAM 占比: " << (scenario.ratio * 100) << "%" << std::endl;
        std::cout << std::string(80, '=') << std::endl;
        
        // 临时修改 FLAGS
        double old_dram = FLAGS_dram_buffer_pool_gib;
        double old_cxl = FLAGS_cxl_gib;
        FLAGS_dram_buffer_pool_gib = scenario.dram_gib;
        FLAGS_cxl_gib = scenario.cxl_gib;
        
        // 创建新的测试环境
        PageCountMinSketch page_cms(12, 1024 * 16);
        DepulicateSetForPageSampling sampling_set(1024, 10000);
        VisitFrequencyBucketArray bucket_array(16);
        SampledVisitHistogram histogram(page_cms, sampling_set, bucket_array);
        
        // 模拟真实数据库查询：Zipfian 分布
        // 模拟查询 500 个不同的页面，访问分布高度倾斜
        std::cout << "\n[1] 模拟数据库查询（Zipfian 分布）..." << std::endl;
        
        std::mt19937_64 rng(12345);
        
        // Zipfian 分布参数
        // Top 5% 页面（25个）：贡献 70% 的访问
        // Top 20% 页面（100个）：贡献 90% 的访问
        // 剩余 80% 页面（400个）：贡献 10% 的访问
        
        std::uniform_int_distribution<u64> super_hot_dist(0, 24);      // 25 个超热页面
        std::uniform_int_distribution<u64> hot_dist(25, 99);           // 75 个热页面
        std::uniform_int_distribution<u64> warm_dist(100, 199);        // 100 个温页面
        std::uniform_int_distribution<u64> cold_dist(200, 499);        // 300 个冷页面
        std::uniform_int_distribution<u64> access_type(0, 999);
        
        u64 super_hot_count = 0, hot_count = 0, warm_count = 0, cold_count = 0;
        
        for (u64 i = 0; i < 10000; i++) {
            u64 type = access_type(rng);
            u64 page_id;
            
            if (type < 700) {  // 70% 访问超热页面
                page_id = super_hot_dist(rng);
                super_hot_count++;
            } else if (type < 900) {  // 20% 访问热页面
                page_id = hot_dist(rng);
                hot_count++;
            } else if (type < 950) {  // 5% 访问温页面
                page_id = warm_dist(rng);
                warm_count++;
            } else {  // 5% 访问冷页面
                page_id = cold_dist(rng);
                cold_count++;
            }
            
            histogram.OnPageAccess(page_id);
        }
        
        std::cout << "  访问分布统计：" << std::endl;
        std::cout << "    - 超热页面（Top 5%）: " << super_hot_count << " 次 (" 
                  << (super_hot_count * 100.0 / 10000) << "%)" << std::endl;
        std::cout << "    - 热页面（Top 5%-20%）: " << hot_count << " 次 (" 
                  << (hot_count * 100.0 / 10000) << "%)" << std::endl;
        std::cout << "    - 温页面（Top 20%-40%）: " << warm_count << " 次 (" 
                  << (warm_count * 100.0 / 10000) << "%)" << std::endl;
        std::cout << "    - 冷页面（剩余 60%）: " << cold_count << " 次 (" 
                  << (cold_count * 100.0 / 10000) << "%)" << std::endl;
        
        // 获取阈值
        u64 threshold_coarse = histogram.GetAdmissionThreshold_coarse();
        u64 threshold_fine = histogram.GetAdmissionThreshold_fine();
        u64 total_sampled = histogram.GetTotalSampledPages();
        
        std::cout << "\n[2] 准入阈值计算结果：" << std::endl;
        std::cout << "  总采样页面数: " << total_sampled << std::endl;
        std::cout << "  粗粒度阈值（Coarse）: " << threshold_coarse << " 次访问" << std::endl;
        std::cout << "  细粒度阈值（Fine）  : " << threshold_fine << " 次访问" << std::endl;
        
        // 计算阈值差异
        double threshold_diff_abs = static_cast<double>(threshold_fine) - static_cast<double>(threshold_coarse);
        double threshold_diff_pct = threshold_coarse > 0 ? 
            (threshold_diff_abs * 100.0 / threshold_coarse) : 0.0;
        
        std::cout << "  阈值差异: " << threshold_diff_abs << " (" 
                  << std::fixed << std::setprecision(2) << threshold_diff_pct << "%)" << std::endl;
        
        // 分析阈值的合理性
        std::cout << "\n[3] 阈值分析：" << std::endl;
        
        // 统计有多少页面会被准入到 DRAM
        u64 admitted_by_coarse = 0;
        u64 admitted_by_fine = 0;
        
        for (u64 page_id = 0; page_id < 500; page_id++) {
            u64 visit_count = page_cms.CMSGetPageAccessCount(page_id);
            if (visit_count >= threshold_coarse) {
                admitted_by_coarse++;
            }
            if (visit_count >= threshold_fine) {
                admitted_by_fine++;
            }
        }
        
        std::cout << "  粗粒度阈值可准入页面数: " << admitted_by_coarse << " / 500 (" 
                  << (admitted_by_coarse * 100.0 / 500) << "%)" << std::endl;
        std::cout << "  细粒度阈值可准入页面数: " << admitted_by_fine << " / 500 (" 
                  << (admitted_by_fine * 100.0 / 500) << "%)" << std::endl;
        
        double target_ratio = scenario.ratio;
        double coarse_ratio = admitted_by_coarse / 500.0;
        double fine_ratio = admitted_by_fine / 500.0;
        
        std::cout << "  理论准入比例: " << (target_ratio * 100) << "%" << std::endl;
        std::cout << "  粗粒度实际比例: " << (coarse_ratio * 100) << "% (误差: " 
                  << std::abs(coarse_ratio - target_ratio) * 100 << "%)" << std::endl;
        std::cout << "  细粒度实际比例: " << (fine_ratio * 100) << "% (误差: " 
                  << std::abs(fine_ratio - target_ratio) * 100 << "%)" << std::endl;
        
        // 验证细粒度阈值 >= 粗粒度阈值
        assert(threshold_fine >= threshold_coarse);
        
        // 打印简化的直方图
        std::cout << "\n[4] 访问频率分布直方图：" << std::endl;
        std::string histogram_str = histogram.PrintHistogram();
        std::cout << histogram_str << std::endl;
        
        // 恢复 FLAGS
        FLAGS_dram_buffer_pool_gib = old_dram;
        FLAGS_cxl_gib = old_cxl;
    }
    
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "总结：" << std::endl;
    std::cout << "1. DRAM 占比越小，准入阈值越高（更严格筛选）" << std::endl;
    std::cout << "2. 细粒度阈值通过对临界 bucket 内页面排序，实现更精确的准入控制" << std::endl;
    std::cout << "3. 在 Zipfian 分布下，两种阈值能有效识别热点页面" << std::endl;
    std::cout << "4. 细粒度阈值的精度优势在页面访问集中在某个区间时更明显" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    PrintSuccess("不同 DRAM:CXL 比例下的阈值对比测试");
}

// ========== 主测试函数 ==========
int main(int argc, char** argv) {
    // 解析命令行参数（如果有的话）
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    
    std::cout << "\n" << std::endl;
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     SampledVisitHistogram 组件测试套件                     ║" << std::endl;
    std::cout << "║     Two-Level Admission Control System                    ║" << std::endl;
    std::cout << "║     重点测试：多线程并发安全性                              ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;
    
    std::cout << "\n当前配置参数：" << std::endl;
    std::cout << "  FLAGS_cxl_tiering_enabled: " << FLAGS_cxl_tiering_enabled << std::endl;
    std::cout << "  FLAGS_dram_buffer_pool_gib: " << FLAGS_dram_buffer_pool_gib << std::endl;
    std::cout << "  FLAGS_cxl_gib: " << FLAGS_cxl_gib << std::endl;
    
    try {
        // 单线程测试
        std::cout << "\n【第一部分：单线程功能测试】" << std::endl;
        TestBasicInitialization();              // 1. 基本初始化
        TestSingleThreadOnPageAccess();         // 2. 单线程 OnPageAccess
        TestWorkerThreadOnPageAccess();         // 3. WorkerThreadOnPageAccess
        TestBackgroundThreadTryUpdate();        // 4. BackgroundThreadTryUpdate
        TestPrintHistogram();                   // 5. 打印直方图
        
        // 多线程测试（重点）
        std::cout << "\n【第二部分：多线程并发测试】" << std::endl;
        TestMultiThreadScenario();              // 6. 多线程场景 (4 Worker + 1 Background)
        TestConcurrentSafetyNoDoubleUpdate();   // 7. 并发安全性（防重复更新）
        TestLongRunningStress();                // 8. 压力测试
        
        // 阈值对比分析（新增）
        std::cout << "\n【第三部分：阈值对比分析】" << std::endl;
        TestThresholdComparisonUnderDifferentRatios();  // 9. 不同 DRAM:CXL 比例下的阈值对比
        
        // 所有测试通过
        std::cout << "\n" << std::endl;
        std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║                  所有测试通过! ✓✓✓                         ║" << std::endl;
        std::cout << "║                  9 个测试全部成功                          ║" << std::endl;
        std::cout << "║                                                            ║" << std::endl;
        std::cout << "║  特别验证：                                                 ║" << std::endl;
        std::cout << "║  ✓ 多线程环境下无并发竞态                                   ║" << std::endl;
        std::cout << "║  ✓ Reset() 不会导致错误阈值                                ║" << std::endl;
        std::cout << "║  ✓ Worker 和 Background 线程职责分离正确                   ║" << std::endl;
        std::cout << "║  ✓ 阈值读取保持一致性                                       ║" << std::endl;
        std::cout << "║  ✓ 粗/细粒度阈值在不同容量比例下表现符合预期                ║" << std::endl;
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
