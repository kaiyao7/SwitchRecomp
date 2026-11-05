# Test build configuration.
# Uses GoogleTest via FetchContent.

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
# For Windows: prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(sr_tests
    unit/test_smoke.cpp
    unit/ir/test_ir_build.cpp
    unit/ir/test_ir_serialize.cpp
    unit/translator/test_decode.cpp
    unit/codegen/test_codegen.cpp
    unit/platform/test_memory.cpp
    unit/platform/test_thread.cpp
    unit/runtime/test_memory_manager.cpp
    unit/runtime/test_vreg.cpp
    unit/runtime/test_svc.cpp
    unit/runtime/test_hle_service.cpp
    unit/runtime/test_loader.cpp
    unit/runtime/test_nso_loader.cpp
    unit/runtime/test_svc_thread.cpp
    unit/gpu/test_command_processor.cpp
    unit/gpu/test_vulkan_backend.cpp
    unit/gpu/test_compute_dispatch.cpp
    integration/translator/test_golden.cpp
    integration/test_e2e_add.cpp
    integration/test_e2e_float.cpp
    integration/test_e2e_fcmp.cpp
    integration/test_e2e_intcmp.cpp
    integration/test_e2e_fp64.cpp
    integration/test_e2e_fp_misc.cpp
    integration/test_e2e_fp_round.cpp
    integration/test_e2e_call.cpp
    integration/test_e2e_image.cpp
    integration/test_e2e_threads.cpp
    integration/test_e2e_simd.cpp
)

target_link_libraries(sr_tests
    PRIVATE
        sr_translator
        sr_codegen
        sr_ir
        sr_platform
        sr_runtime
        sr_gpu
        sr_common
        GTest::gtest_main
)

# Let tests reach internal runtime headers (e.g. memory_manager.h).
target_include_directories(sr_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Let golden tests locate fixtures regardless of the working directory.
target_compile_definitions(sr_tests PRIVATE
    SR_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
    SR_INCLUDE_DIR="${SR_INCLUDE_DIR}"
)

include(GoogleTest)
gtest_discover_tests(sr_tests)
