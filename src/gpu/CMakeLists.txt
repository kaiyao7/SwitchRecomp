# Vulkan is optional: when the SDK/loader is found the backend is compiled
# with real Vulkan support (SR_HAS_VULKAN); otherwise it still builds and
# Initialize() reports an explicit error.
find_package(Vulkan QUIET)

# --- Compute shader generation (Phase 4-1) -------------------------------
# Regenerate SPIR-V artifacts at configure time. The generator is
# idempotent and keeps prebuilt artifacts when no shader compiler exists.
set(SR_GPU_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated)

find_program(SR_PYTHON_EXECUTABLE NAMES python python3)
if(SR_PYTHON_EXECUTABLE)
    set(_sr_shader_args)
    if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
        list(APPEND _sr_shader_args
             --glslang "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}")
    endif()
    if(Vulkan_GLSLC_EXECUTABLE)
        list(APPEND _sr_shader_args
             --glslc "${Vulkan_GLSLC_EXECUTABLE}")
    endif()

    execute_process(
        COMMAND "${SR_PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/scripts/gen_shaders.py"
                ${_sr_shader_args}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _sr_shader_result
        OUTPUT_VARIABLE _sr_shader_output
        ERROR_VARIABLE _sr_shader_output
    )
    message(STATUS "gen_shaders.py:\n${_sr_shader_output}")
    if(NOT _sr_shader_result EQUAL 0)
        message(WARNING "gen_shaders.py failed (code ${_sr_shader_result})")
    endif()
else()
    message(STATUS "python not found; using prebuilt shader artifacts if present")
endif()

if(EXISTS "${SR_GPU_GENERATED_DIR}/add1_spv.h")
    set(SR_GPU_HAS_SHADERS TRUE)
else()
    set(SR_GPU_HAS_SHADERS FALSE)
    message(STATUS "no embedded compute shaders; compute dispatch tests will skip")
endif()

set(_sr_gpu_sources
    command_processor.cpp
    vulkan_backend.cpp
)
# The executor translation unit requires Vulkan headers.
if(Vulkan_FOUND)
    list(APPEND _sr_gpu_sources compute_executor.cpp)
endif()

add_library(sr_gpu STATIC ${_sr_gpu_sources})
target_include_directories(sr_gpu PUBLIC ${SR_INCLUDE_DIR})
target_link_libraries(sr_gpu PUBLIC sr_runtime sr_platform sr_common)

if(Vulkan_FOUND)
    target_compile_definitions(sr_gpu PUBLIC SR_HAS_VULKAN=1)
    target_link_libraries(sr_gpu PUBLIC Vulkan::Vulkan)
    # Internal headers live next to the sources; generated headers in the
    # generated/ subdirectory.
    target_include_directories(sr_gpu PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SR_GPU_GENERATED_DIR}
    )
    if(SR_GPU_HAS_SHADERS)
        target_compile_definitions(sr_gpu PUBLIC SR_GPU_HAS_SHADERS=1)
    endif()
endif()
