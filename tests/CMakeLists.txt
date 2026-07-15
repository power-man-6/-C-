add_executable(threadpool_test test_basic.cpp)
target_link_libraries(threadpool_test threadpool)
add_test(NAME ThreadPool.Basic COMMAND threadpool_test)
