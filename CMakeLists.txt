# 要求 CMake 最低版本为 3.16
cmake_minimum_required(VERSION 3.16)

# 定义项目名称、版本号和使用的编程语言
project(cpp-threadpool VERSION 1.0.0 LANGUAGES CXX)

# 设置 C++ 标准为 C++17
set(CMAKE_CXX_STANDARD 17)
# 要求编译器必须支持 C++17（不支持时报错而非降级）
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 关闭编译器扩展，确保代码严格符合 C++ 标准
set(CMAKE_CXX_EXTENSIONS OFF)

# 构建选项：是否编译示例程序（默认开启）
option(BUILD_EXAMPLES "Build example programs" ON)
# 构建选项：是否编译测试程序（默认开启）
option(BUILD_TESTS "Build tests" ON)

# 创建一个 INTERFACE 库（header-only 库无需编译，仅提供头文件路径和编译要求）
add_library(threadpool INTERFACE)
# 指定头文件搜索路径为 include 目录
target_include_directories(threadpool INTERFACE include)
# 要求使用此库的目标必须以 C++17 标准编译
target_compile_features(threadpool INTERFACE cxx_std_17)

# 如果开启了示例程序构建，则添加 examples 子目录
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# 如果开启了测试构建，则启用 CTest 并添加 tests 子目录
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
