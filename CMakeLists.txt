cmake_minimum_required(VERSION 3.20)
project(SwitchRecomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Common include directory
set(SR_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

# Modules (each is an independent static library target)
add_subdirectory(src/common)
add_subdirectory(src/platform)
add_subdirectory(src/ir)
add_subdirectory(src/translator)
add_subdirectory(src/codegen)
add_subdirectory(src/runtime)
add_subdirectory(src/gpu)

# Tools
add_subdirectory(tools)

# Tests
enable_testing()
add_subdirectory(tests)

# Benchmarks
option(SR_BUILD_BENCHMARKS "Build benchmarks" ON)
if(SR_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
