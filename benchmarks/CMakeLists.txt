include(FetchContent)
FetchContent_Declare(
  google_benchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(google_benchmark)

add_executable(bench_translator bench_translator.cpp)
target_link_libraries(bench_translator PRIVATE sr_translator sr_ir sr_common benchmark::benchmark)

add_executable(bench_codegen bench_codegen.cpp)
target_link_libraries(bench_codegen PRIVATE sr_codegen sr_ir sr_common benchmark::benchmark)

add_executable(bench_memory bench_memory.cpp)
target_link_libraries(bench_memory PRIVATE sr_runtime sr_platform sr_common benchmark::benchmark)

# Numeric-substrate throughput: validates the "inline -> zero overhead" claim.
add_executable(bench_numeric bench_numeric.cpp)
target_link_libraries(bench_numeric PRIVATE sr_common benchmark::benchmark)
