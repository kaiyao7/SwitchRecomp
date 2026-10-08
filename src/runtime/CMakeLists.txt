add_library(sr_runtime STATIC
    runtime.cpp
    memory_manager.cpp
    svc_table.cpp
    svc_thread.cpp
    hle_service.cpp
    elf_loader.cpp
    nro_loader.cpp
    loader_common.cpp
)
target_include_directories(sr_runtime PUBLIC ${SR_INCLUDE_DIR})
target_link_libraries(sr_runtime PUBLIC sr_platform sr_ir sr_common)
