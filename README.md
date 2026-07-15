# cpp-threadpool

一个基于工作窃取（work-stealing）的高性能 header-only C++17 线程池。

## 特性

- **Header-only** -- 单个 `#include` 即可使用，除 C++ 标准库外零依赖。
- **工作窃取调度器** -- 每个工作线程拥有私有的双端队列。生产者将任务推入队列头部；空闲工作线程从兄弟队列的尾部窃取任务。这种设计在高负载下将锁竞争降至最低。
- **固定大小线程池** -- 工作线程数量在构造时指定（默认值为 `std::thread::hardware_concurrency()`）。
- **Future 与异常传递** -- `enqueue()` 返回 `std::future<R>`。任务内部抛出的异常会通过 future 传播给调用方。
- **线程安全的提交** -- 在线程池存活期间，可在任意时刻从任意线程调用 `enqueue()` / `submit()`。
- **优雅关闭** -- 析构函数会在 join 工作线程之前排空所有待处理任务。不会静默丢弃任何任务。
- **轻量级监控** -- `size()` 返回工作线程数量；`pending()` 返回未执行任务的近似计数。
- **现代 C++17** -- 内部使用了折叠表达式和 `if constexpr`。

## 环境要求

- CMake >= 3.16
- 支持 C++17 的编译器（GCC 8+、Clang 7+、MSVC 2017 15.7+）

## 快速开始

```cpp
#include <threadpool/threadpool.hpp>
#include <iostream>

int main() {
    thp::thread_pool pool;                  // 自动调整为硬件并发数

    auto future = pool.enqueue([](int a, int b) {
        return a + b;
    }, 3, 4);

    std::cout << "3 + 4 = " << future.get() << "\n";  // 输出 "7"
    return 0;
}
```

## 构建

```bash
cd cpp-threadpool
mkdir build && cd build
cmake ..
cmake --build .
```

### 构建选项

| 选项              | 默认值 | 说明                     |
|-------------------|--------|--------------------------|
| `BUILD_EXAMPLES`  | ON     | 构建示例程序             |
| `BUILD_TESTS`     | ON     | 构建并注册测试           |

向 cmake 传递 `-DBUILD_EXAMPLES=OFF` 或 `-DBUILD_TESTS=OFF` 可禁用相应构建。

### 运行示例

```bash
./examples/threadpool_example          # Linux / macOS
examples\Debug\threadpool_example.exe  # Windows (Visual Studio)
```

### 运行测试

```bash
ctest                         # 通过 CTest 运行
./tests/threadpool_test       # 直接运行测试可执行文件
```

## API 参考

### `thp::thread_pool`

```cpp
namespace thp {
class thread_pool {
public:
    explicit thread_pool(std::size_t num_threads = 0);

    // 禁止拷贝和移动
    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    ~thread_pool();  // 排空所有待处理任务，然后 join 所有工作线程

    // 提交可调用对象 + 参数，返回 future。
    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>,
                                            std::decay_t<Args>...>>;

    // enqueue() 的同义词。
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>,
                                            std::decay_t<Args>...>>;

    std::size_t size() const noexcept;     // 返回工作线程数量
    std::size_t pending() const noexcept;  // 返回待处理任务的近似数量
};
}
```

### `thread_pool(std::size_t num_threads = 0)`

创建一个包含 `num_threads` 个工作线程的线程池。若 `num_threads` 为 0（默认值），则使用 `std::thread::hardware_concurrency()`，下限为 1。

### `enqueue(F&& f, Args&&... args)` / `submit(F&& f, Args&&... args)`

向线程池提交任务。任务通过轮询（round-robin）方式调度到工作线程的队列中。调用方收到一个持有返回值的 `std::future`。

- 若 `f` 返回值，`future.get()` 返回该值。
- 若 `f` 返回 `void`，`future.get()` 返回 `void`（除非任务抛出异常，否则不抛异常）。
- 若 `f` 抛出异常，该异常会从 `future.get()` 中重新抛出。

### `size()`

返回线程池中的工作线程数量（在线程池的整个生命周期中保持不变）。

### `pending()`

返回已提交但尚未执行完成的任务的近似数量。这是一个用于监控的启发式方法；不保证在任意时刻都精确。

## 设计细节

### 调度器

任务提交时以轮询方式分发到各工作线程的队列中。每个工作线程优先处理自己队列中的任务（FIFO 先进先出）。当工作线程的队列为空时，它会以伪随机顺序扫描兄弟工作线程的队列，从每个目标队列的**尾部**（LIFO 后进先出端）窃取任务。

这种 FIFO/LIFO 双端队列安排——有时被称为"工作窃取双端队列"——与 Intel TBB 和 Java ForkJoinPool 调度器采用的策略相同。队列拥有者从头部推入和弹出任务；窃取者从尾部弹出任务。竞争仅限于每个双端队列的共享 `std::mutex`，而头部与尾部分离的访问模式保持了低竞争水平。

### 关闭过程

析构函数执行时：

1. 设置原子的停止标志位。
2. 通知每个工作线程的条件变量。
3. 析构函数 join 所有工作线程。
4. 在最后的循环迭代中，工作线程排空自己队列中剩余的所有任务，确保工作零丢失。

不会丢弃任何任务，也不会在析构函数返回后执行任何任务。

### 线程安全性

- `enqueue()` / `submit()` 仅获取目标工作线程队列的互斥锁。多个生产者可以同时向不同工作线程的队列提交任务而互不竞争。
- 工作线程获取自己队列的互斥锁来弹出任务，获取兄弟队列的互斥锁来窃取任务。常见情况（从自己队列弹出）是一个较短的临界区。
- `pending()` 读取 `std::atomic<int64_t>`，不阻塞。

## 性能

在 16 核（32 线程）机器上，对于轻量级任务（单次整数加法），线程池可维持约 800~1200 万次/秒的任务提交速率。具体吞吐量取决于任务粒度和硬件线程数量。对于粗粒度任务（数百微秒以上），调度开销可忽略不计。

## 许可证

MIT -- 详见 [LICENSE](LICENSE) 文件。
