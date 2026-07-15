/// \file examples/main.cpp
/// \brief 全面展示 thread_pool API 的示例程序。
///
/// 构建方法（在项目根目录下执行）：
///   mkdir build && cd build && cmake .. && cmake --build .
///
/// 运行方法：
///   ./examples/threadpool_example         （Linux / macOS）
///   .\examples\Debug\threadpool_example.exe （Windows）

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <threadpool/threadpool.hpp>

// -----------------------------------------------------------------------
// 辅助函数：返回人类可读的日志时间戳（格式：HH:MM:SS.mmm）
// -----------------------------------------------------------------------
static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t    = std::chrono::system_clock::to_time_t(now);
    auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%T") << '.'
        << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

// -----------------------------------------------------------------------
// 一个纯粹的计算密集型任务：递归计算斐波那契数列
// -----------------------------------------------------------------------
static std::uint64_t fibonacci(std::uint64_t n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

// -----------------------------------------------------------------------
// 模拟 I/O 密集型任务：在指定毫秒内"假装工作"（sleep）
// -----------------------------------------------------------------------
static void simulate_io(int id, int duration_ms) {
    std::ostringstream oss;
    oss << "[" << timestamp() << "]   IO 任务 #" << id
        << " 在线程 " << std::this_thread::get_id()
        << " 上开始执行（预计耗时 " << duration_ms << " ms）";
    std::cout << oss.str() << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    oss.str("");
    oss << "[" << timestamp() << "]   IO 任务 #" << id
        << " 在线程 " << std::this_thread::get_id()
        << " 上执行完毕";
    std::cout << oss.str() << std::endl;
}

// =======================================================================
int main() {
    std::cout << "========== 线程池示例 ==========\n"
              << "硬件并发数: "
              << std::thread::hardware_concurrency() << "\n\n";

    // -----------------------------------------------------------------
    // 1. 创建线程池（自动调整为 hardware_concurrency 个线程）
    // -----------------------------------------------------------------
    thp::thread_pool pool;
    std::cout << "线程池已创建，包含 " << pool.size() << " 个工作线程。\n\n";

    // -----------------------------------------------------------------
    // 2. 提交多个计算密集型的斐波那契任务
    // -----------------------------------------------------------------
    std::cout << "--- 计算密集型任务（斐波那契） ---\n";
    std::vector<std::future<std::uint64_t>> fib_futures;
    fib_futures.reserve(8);
    // n = 40 在现代硬件上大约耗时 0.2~1 秒——足够观察到并行效果，
    // 又不至于让演示过久。
    for (int i = 0; i < 8; ++i) {
        fib_futures.push_back(pool.enqueue(fibonacci, 40ULL));
    }

    std::cout << "已提交全部 8 个 fibonacci(40) 任务，等待结果...\n";
    int idx = 0;
    for (auto& fut : fib_futures) {
        std::cout << "  fib(" << 40 << ") = " << fut.get() << "\n";
        ++idx;
    }
    std::cout << "计算密集型批次执行完毕。\n\n";

    // -----------------------------------------------------------------
    // 3. 混合各种耗时的模拟 I/O 任务
    // -----------------------------------------------------------------
    std::cout << "--- 模拟 I/O 任务 ---\n";
    std::vector<std::future<void>> io_futures;
    io_futures.reserve(12);
    for (int i = 0; i < 12; ++i) {
        // 每个任务依次增加 20ms 耗时，展示不同执行时长
        io_futures.push_back(
            pool.enqueue(simulate_io, i, /*duration_ms=*/50 + i * 20));
    }
    std::cout << "已提交全部 12 个 I/O 任务。\n\n";

    // 等待所有 I/O 任务完成
    for (auto& fut : io_futures) {
        fut.get();
    }
    std::cout << "\n所有 I/O 任务已完成。\n\n";

    // -----------------------------------------------------------------
    // 4. 演示 pending() 监控功能
    // -----------------------------------------------------------------
    std::cout << "--- 待处理任务数量监控 ---\n";
    constexpr int kMany = 200;
    std::vector<std::future<void>> monitoring_futures;
    monitoring_futures.reserve(kMany);
    for (int i = 0; i < kMany; ++i) {
        monitoring_futures.push_back(pool.enqueue([&pool] {
            // 微小的自旋操作来模拟工作负载
            volatile int x = 0;
            for (int j = 0; j < 10000; ++j) x += j;
        }));
    }
    // 在任务处理过程中定期打印待处理数量
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::cout << "  pending() ~ " << pool.pending() << "\n";
    }
    for (auto& fut : monitoring_futures) fut.get();
    std::cout << "  全部完成后 pending(): " << pool.pending() << "\n\n";

    // -----------------------------------------------------------------
    // 5. 展示线程池在一批任务结束后可以重复使用
    // -----------------------------------------------------------------
    std::cout << "--- 线程池复用 ---\n";
    auto f1 = pool.enqueue([] { return 42; });
    auto f2 = pool.enqueue([](int a, int b) { return a + b; }, 7, 8);
    std::cout << "  f1 = " << f1.get() << ", f2 = " << f2.get() << "\n\n";

    // -----------------------------------------------------------------
    // 6. 演示任务被分配到不同的线程上执行
    // -----------------------------------------------------------------
    std::cout << "--- 线程分配情况演示 ---\n";
    std::mutex print_mtx;
    constexpr int kThreadDemo = 20;
    std::vector<std::future<void>> dist_futures;
    dist_futures.reserve(kThreadDemo);
    for (int i = 0; i < kThreadDemo; ++i) {
        dist_futures.push_back(pool.enqueue([i, &print_mtx] {
            std::lock_guard<std::mutex> lk(print_mtx);
            std::cout << "  任务 " << i << " 在线程 "
                      << std::this_thread::get_id() << " 上运行\n";
        }));
    }
    for (auto& fut : dist_futures) fut.get();
    std::cout << "（观察上面的线程 ID——它们应该各不相同）\n\n";

    // -----------------------------------------------------------------
    // 7. 析构函数优雅地排空所有剩余任务
    // -----------------------------------------------------------------
    std::cout << "--- 优雅关闭 ---\n"
              << "线程池即将被销毁。所有剩余任务将在工作线程\n"
              << "被 join 之前被排空执行。\n";

    // 析构函数在此处运行（main 结束时 pool 离开作用域）。
    return 0;
}
