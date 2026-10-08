add_library(sr_platform STATIC
    memory.cpp
    thread.cpp
)
target_include_directories(sr_platform PUBLIC ${SR_INCLUDE_DIR})
target_link_libraries(sr_platform PUBLIC sr_common)
