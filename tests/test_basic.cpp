/// \file tests/test_basic.cpp
/// \brief thread_pool 的基本正确性测试。
///
/// 这是一个轻量级测试框架——不需要任何外部测试库。
/// 非零退出码表示测试失败；零退出码并在 stdout 输出
/// "All tests passed." 表示测试全部通过。

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <threadpool/threadpool.hpp>

// -----------------------------------------------------------------------
// 极简测试基础设施
// -----------------------------------------------------------------------
static int g_failures = 0;  ///< 全局失败计数器

/// 断言宏：若条件不成立，则打印失败信息并增加失败计数。
#define EXPECT(cond, msg)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__       \
                      << "]: " << msg << "\n";                         \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

// -----------------------------------------------------------------------
// 测试 1：最基本的单任务执行
// -----------------------------------------------------------------------
static void test_single_task() {
    std::cout << "test_single_task ... ";
    thp::thread_pool pool(4);
    auto fut = pool.enqueue([] { return 42; });
    EXPECT(fut.get() == 42, "期望返回 42");
    std::cout << "OK\n";
}

// -----------------------------------------------------------------------
// 测试 2：带参数的任务 + void 返回类型
// -----------------------------------------------------------------------
static void test_task_with_args() {
    std::cout << "test_task_with_args ... ";

    thp::thread_pool pool(4);

    // 非 void 返回值
    auto f1 = pool.enqueue([](int a, int b) { return a * b; }, 6, 7);
    EXPECT(f1.get() == 42, "6 * 7 应该等于 42");

    // void 返回值——仅检查 future 是否在不抛异常的情况下完成。
    bool flag = false;
    auto f2 = pool.enqueue([&flag] { flag = true; });
    f2.get();
    EXPECT(flag, "void 任务应该已将 flag 设为 true");

    std::cout << "OK\n";
}

// -----------------------------------------------------------------------
// 测试 3：大量任务，全部正确完成
// -----------------------------------------------------------------------
static void test_many_tasks() {
    std::cout << "test_many_tasks ... ";

    constexpr int kNumTasks = 2000;
    thp::thread_pool pool(4);
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(kNumTasks);

    for (int i = 0; i < kNumTasks; ++i) {
        futures.push_back(pool.enqueue([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& f : futures) f.get();
    EXPECT(counter.load() == kNumTasks,
           "所有任务都应该已递增计数器");

    std::cout << "OK\n";
}

// -----------------------------------------------------------------------
// 测试 4：验证确实使用了多个线程
// -----------------------------------------------------------------------
static void test_multiple_threads_used() {
    std::cout << "test_multiple_threads_used ... ";

    constexpr std::size_t kWorkers = 8;
    thp::thread_pool pool(kWorkers);
    std::atomic<int> unique_threads{0};
    std::mutex mtx;
    std::set<std::thread::id> ids;  ///< 收集所有出现过的线程 ID

    constexpr int kTasks = 100;
    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.enqueue([&] {
            std::lock_guard<std::mutex> lk(mtx);
            ids.insert(std::this_thread::get_id());
        }));
    }
    for (auto& f : futures) f.get();

    std::cout << "(使用了 " << ids.size() << " / " << kWorkers
              << " 个线程) ";

    // 100 个任务交给 8 个工作线程，几乎可以保证至少有 2 个不同的线程
    // 执行了任务。实际运行中通常 8 个都会被用到。
    EXPECT(ids.size() >= 2,
           "至少应有 2 个不同的线程执行过任务");

    std::cout << "OK\n";
}

// -----------------------------------------------------------------------
// 测试 5：压力测试——10,000 个任务
// -----------------------------------------------------------------------
static void test_stress() {
    std::cout << "test_stress (10000 个任务) ... ";

    constexpr int kNumTasks = 10000;
    thp::thread_pool pool;          // 使用 hardware_concurrency 个工作线程
    std::atomic<std::int64_t> sum{0};
    std::vector<std::future<void>> futures;
    futures.reserve(kNumTasks);

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < kNumTasks; ++i) {
        futures.push_back(pool.enqueue([&sum, i] {
            sum.fetch_add(i, std::memory_order_relaxed);
        }));
    }

    for (auto& f : futures) f.get();

    auto end   = std::chrono::steady_clock::now();
    auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                   end - start).count();

    // 0 到 9999 的累加和：n*(n-1)/2
    constexpr std::int64_t kExpected =
        static_cast<std::int64_t>(kNumTasks) * (kNumTasks - 1) / 2;
    EXPECT(sum.load() == kExpected,
           "累加和应等于 n*(n-1)/2");

    std::cout << "OK (" << ms << " ms)\n";
}

// -----------------------------------------------------------------------
// 测试 6：待处理计数器随任务完成而递减
// -----------------------------------------------------------------------
static void test_pending_counter() {
    std::cout << "test_pending_counter ... ";

    thp::thread_pool pool(2);
    constexpr int kTasks = 500;
    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.enqueue([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }));
    }

    // 让部分任务先行完成，然后检查 pending() 是否非零
    // （如果我们提交得够快，可能已经归零）。
    // 核心不变条件是：所有 future 就绪后，pending() == 0。
    for (auto& f : futures) f.get();
    EXPECT(pool.pending() == 0,
           "所有任务完成后 pending 应为 0");

    std::cout << "OK\n";
}

// -----------------------------------------------------------------------
// 测试 7：单工作线程的线程池（锤炼工作窃取的边界路径）
// -----------------------------------------------------------------------
static void test_single_worker() {
    std::cout << "test_single_worker ... ";

    thp::thread_pool pool(1);
    auto f = pool.enqueue([] { return 99; });
    EXPECT(f.get() == 99, "单工作线程应返回 99");

    // 将大量任务入队——它们都在单一线程上顺序执行。
    constexpr int N = 500;
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(N);
    for (int i = 0; i < N; ++i) {
        futures.push_back(pool.enqueue([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    for (auto& f : futures) f.get();
    EXPECT(counter.load() == N, "所有任务应在 1 个工作线程上全部完成");

    std::cout << "OK\n";
}

// -----------------------------------------------------------------------
// 测试 8：submit() 同义词与 enqueue() 行为完全一致
// -----------------------------------------------------------------------
static void test_submit_synonym() {
    std::cout << "test_submit_synonym ... ";

    thp::thread_pool pool(2);
    auto f = pool.submit([](std::string a, std::string b) {
        return a + " " + b;
    }, "hello", "world");
    EXPECT(f.get() == "hello world", "submit 应与 enqueue 行为一致");

    std::cout << "OK\n";
}

// =======================================================================
int main() {
    // 依次执行所有测试
    test_single_task();
    test_task_with_args();
    test_many_tasks();
    test_multiple_threads_used();
    test_stress();
    test_pending_counter();
    test_single_worker();
    test_submit_synonym();

    std::cout << "\n";
    if (g_failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    } else {
        std::cout << g_failures << " test(s) FAILED.\n";
        return 1;
    }
}
