add_library(sr_ir STATIC ir.cpp)
target_include_directories(sr_ir PUBLIC ${SR_INCLUDE_DIR})
target_link_libraries(sr_ir PUBLIC sr_common sr_platform)
