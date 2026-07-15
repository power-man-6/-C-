/// \file threadpool.hpp
/// \brief 一个基于工作窃取（work-stealing）的高性能 header-only C++17 线程池。
///
/// 设计概述：
/// - 每个工作线程拥有一个私有任务队列（std::deque），由互斥锁保护。
///   任务通过全局的"下一个工作线程"槽位以轮询（round-robin）方式提交，
///   目标是均匀分配负载。
/// - 当某个工作线程发现自己的队列为空时，它会从伪随机偏移量开始扫描
///   兄弟队列（工作窃取）。窃取时从目标线程双端队列的**尾部**取出任务，
///   而目标线程自己从**头部**推入/消费任务——这种策略将锁竞争降到最低。
/// - 关闭过程是协作式的：析构函数先排空所有任务，再发出停止信号，
///   唤醒所有线程并 join。不会遗漏任何任务。
///
/// 线程安全：
/// - 在线程池存活期间，submit() 和 enqueue() 可以被任意线程在任意时刻安全调用。
/// - pending() 和 size() 是无锁的，或仅获取轻量级原子快照；
///   它们保证安全但不保证精确（近似值）。

#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <type_traits>
#include <vector>

namespace thp {            // "thp" = THreadPool，为简洁起见使用短命名

// ===========================================================================
// 内部辅助工具：编译期检测 void 返回类型的小工具
// ===========================================================================
namespace detail {

/// 调用可调用对象并将结果设置到对应的 promise 中。
/// 非 void 返回类型的主模板。
template <typename R, typename F, typename... Args>
void set_promise(std::promise<R>& p, F&& f, Args&&... args) {
    if constexpr (std::is_void_v<R>) {
        // 返回类型为 void：先执行函数，再设置空值
        std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
        p.set_value();
    } else {
        // 返回类型非 void：执行函数并将返回值传入 promise
        p.set_value(
            std::invoke(std::forward<F>(f), std::forward<Args>(args)...));
    }
}

} // namespace detail

// ===========================================================================
// thread_pool —— 线程池的公开接口类
// ===========================================================================
class thread_pool {
public:
    // ------------------------------------------------------------------
    // 构造 / 析构
    // ------------------------------------------------------------------

    /// 创建一个包含 \p num_threads 个工作线程的线程池。
    /// 若 \p num_threads == 0，则自动使用
    /// std::thread::hardware_concurrency()（但不会少于 1）。
    explicit thread_pool(std::size_t num_threads = 0)
        : stop_flag_(false)
        , pending_count_(0)
    {
        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) num_threads = 1;   // 兜底：若硬件并发数也取不到，则使用 1
        }

        // 根据线程数量分配对应的队列、互斥锁和条件变量
        queues_.resize(num_threads);
        mutexes_.resize(num_threads);
        cvs_.resize(num_threads);

        // 创建工作线程，每个线程执行 worker_loop 主循环
        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back(&thread_pool::worker_loop, this, i);
        }
    }

    /// 禁止拷贝和移动。
    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool(thread_pool&&) = delete;
    thread_pool& operator=(thread_pool&&) = delete;

    /// 析构函数：优雅地关闭所有工作线程，在此之前会排空所有待处理任务。
    ~thread_pool() {
        shutdown();
    }

    // ------------------------------------------------------------------
    // 公开 API
    // ------------------------------------------------------------------

    /// 返回线程池中工作线程的数量。
    std::size_t size() const noexcept { return workers_.size(); }

    /// 返回**近似**的等待执行任务数量。
    /// 这是一个用于监控的启发式方法；不要依赖它来保证程序的正确性。
    std::size_t pending() const noexcept {
        return static_cast<std::size_t>(
            pending_count_.load(std::memory_order_relaxed));
    }

    /// 提交一个可调用对象及其参数，返回一个 std::future。
    /// 任务会被调度到其中一个工作线程上执行。
    ///
    /// \note 即便在线程池销毁前任务尚未执行，返回的 future 仍然有效——
    ///       析构函数会排空所有任务。
    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>,
                                            std::decay_t<Args>...>>;

    /// enqueue() 的同义词——方便在使用模板推导时调用。
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>,
                                            std::decay_t<Args>...>>
    {
        return enqueue(std::forward<F>(f), std::forward<Args>(args)...);
    }

private:
    // ------------------------------------------------------------------
    // 工作线程底层实现
    // ------------------------------------------------------------------

    /// 任务的类型别名：一个无参数、无返回值的可调用对象。
    using task_type = std::function<void()>;

    /// 每个工作线程执行的主循环。
    void worker_loop(std::size_t worker_id);

    /// 尝试从**自己**队列的头部取出一个任务。
    /// 成功时返回 true 并将任务赋值给 \p task。
    bool pop_own(std::size_t id, task_type& task);

    /// 尝试从**兄弟**队列的尾部窃取一个任务。
    /// 成功时返回 true 并将任务赋值给 \p task。
    bool steal(std::size_t victim_id, task_type& task);

    /// 协作式关闭：排空剩余任务，然后 join 所有线程。
    void shutdown();

    // ------------------------------------------------------------------
    // 数据成员
    // ------------------------------------------------------------------

    std::vector<std::thread> workers_;                   ///< 工作线程列表
    std::vector<std::deque<task_type>> queues_;          ///< 每个工作线程私有的任务双端队列
    std::vector<std::mutex> mutexes_;                    ///< 每个队列对应的互斥锁
    std::vector<std::condition_variable> cvs_;           ///< 每个工作线程的条件变量（用于阻塞等待）

    std::atomic<bool> stop_flag_;                        ///< 停止标志：为 true 时工作线程应退出循环
    std::atomic<std::int64_t> pending_count_;            ///< 待处理任务的近似计数

    /// 任务提交的轮询游标（永远不会重置——对 u64 来说不成问题）。
    std::atomic<std::uint64_t> next_worker_{0};
};

// ===========================================================================
// 模板实现（必须放在头文件中，因为这是 header-only 库）
// ===========================================================================

template <typename F, typename... Args>
auto thread_pool::enqueue(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>,
                                        std::decay_t<Args>...>>
{
    using return_type =
        std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    // 将可调用对象及其参数封装到 std::packaged_task 中，
    // 这样我们可以立即向外提供一个 future。
    auto task_ptr = std::make_shared<std::packaged_task<return_type()>>(
        [fn = std::forward<F>(f),
         ... args = std::forward<Args>(args)]() mutable -> return_type {
            if constexpr (std::is_void_v<return_type>) {
                // 返回类型为 void：直接执行，不返回值
                std::invoke(std::forward<F>(fn),
                            std::forward<Args>(args)...);
            } else {
                // 返回类型非 void：执行并返回结果
                return std::invoke(std::forward<F>(fn),
                                   std::forward<Args>(args)...);
            }
        });

    std::future<return_type> result = task_ptr->get_future();

    // 如果线程池已经在关闭过程中，则提前退出——不再接受新任务
    // （否则析构函数会因等待 pending_count 归零而死锁）。
    if (stop_flag_.load(std::memory_order_acquire)) {
        // 此时 future 将永远无法被满足，调用方会收到 broken promise 异常。
        // 这是在关闭期间入队的一致行为——不要在关闭时提交任务。
        return result;
    }

    // 在将任务推入队列**之前**增加待处理计数器，
    // 这样析构函数的排空循环才能看到正确的总数。
    pending_count_.fetch_add(1, std::memory_order_relaxed);

    // 通过轮询（round-robin）选择一个目标工作线程队列。
    const std::size_t n = queues_.size();
    const std::size_t target =
        static_cast<std::size_t>(
            next_worker_.fetch_add(1, std::memory_order_relaxed) % n);

    {
        // 锁定目标队列，将任务推到该队列末尾
        std::lock_guard<std::mutex> lk(mutexes_[target]);
        queues_[target].emplace_back(
            [t = std::move(task_ptr)]() mutable { (*t)(); });
    }
    // 通知目标工作线程有新任务到达
    cvs_[target].notify_one();

    return result;
}

// ---------------------------------------------------------------------------
// worker_loop —— 每个工作线程的主循环
// ---------------------------------------------------------------------------
inline void thread_pool::worker_loop(std::size_t worker_id) {
    // 每个工作线程拥有自己的随机数生成器，用于伪随机窃取顺序。
    // 我们用工作线程 ID 与高精度时钟滴答做异或混合，
    // 以避免各线程之间产生相关的随机序列。
    std::mt19937_64 rng(
        static_cast<std::uint64_t>(worker_id) ^
        static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now()
                .time_since_epoch()
                .count()));

    for (;;) {
        task_type task;

        // 第 1 步：优先尝试从自己的队列获取任务。
        if (pop_own(worker_id, task)) {
            task();  // 执行任务
            pending_count_.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }

        // 第 2 步：在阻塞之前检查停止信号。
        if (stop_flag_.load(std::memory_order_acquire)) break;

        // 第 3 步：尝试从兄弟队列窃取任务（工作窃取）。
        const std::size_t n = workers_.size();
        if (n > 1) {
            // 选择一个随机起始偏移量，避免所有窃取者同时争抢工作线程 0。
            std::uniform_int_distribution<std::size_t> dist(0, n - 2);
            const std::size_t start = dist(rng);
            bool stolen = false;
            // 遍历除自己以外的所有其他队列
            for (std::size_t k = 0; k < n - 1; ++k) {
                const std::size_t victim =
                    (worker_id + 1 + start + k) % n;
                if (steal(victim, task)) {
                    stolen = true;
                    break;
                }
            }
            if (stolen) {
                task();  // 执行窃取到的任务
                pending_count_.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }
        }

        // 第 4 步：无事可做——在自己的条件变量上休眠等待
        //         （等待新任务到达或收到停止信号）。
        {
            std::unique_lock<std::mutex> lk(mutexes_[worker_id]);
            cvs_[worker_id].wait(lk, [&] {
                return stop_flag_.load(std::memory_order_acquire) ||
                       !queues_[worker_id].empty();
            });
        }
    }

    // 收到停止信号后，工作线程排空自己的队列，
    // 确保不遗漏任何任务。
    for (;;) {
        task_type task;
        if (!pop_own(worker_id, task)) break;
        task();
        pending_count_.fetch_sub(1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// pop_own —— 从工作线程自己的双端队列**头部**取出任务
// ---------------------------------------------------------------------------
inline bool thread_pool::pop_own(std::size_t id, task_type& task) {
    std::lock_guard<std::mutex> lk(mutexes_[id]);
    if (queues_[id].empty()) return false;
    task = std::move(queues_[id].front());
    queues_[id].pop_front();
    return true;
}

// ---------------------------------------------------------------------------
// steal —— 从兄弟队列的**尾部**窃取任务（将锁竞争降至最低）
// ---------------------------------------------------------------------------
inline bool thread_pool::steal(std::size_t victim_id, task_type& task) {
    std::lock_guard<std::mutex> lk(mutexes_[victim_id]);
    if (queues_[victim_id].empty()) return false;
    task = std::move(queues_[victim_id].back());
    queues_[victim_id].pop_back();
    return true;
}

// ---------------------------------------------------------------------------
// shutdown —— 协作式关闭过程
// ---------------------------------------------------------------------------
inline void thread_pool::shutdown() {
    // 步骤 1：向所有工作线程发出停止信号。
    stop_flag_.store(true, std::memory_order_release);

    // 步骤 2：唤醒所有工作线程（以防它们在条件变量上休眠）。
    for (std::size_t i = 0; i < cvs_.size(); ++i) {
        cvs_[i].notify_all();
    }

    // 步骤 3：等待所有工作线程退出（join）。
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }

    // 此时 pending_count_ 必须为零——所有任务已被工作线程在其
    // 最后的循环迭代中排空。
    assert(pending() == 0 && "Tasks leaked during shutdown");
}

} // namespace thp
