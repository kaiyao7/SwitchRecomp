add_library(sr_codegen STATIC codegen.cpp)
target_include_directories(sr_codegen PUBLIC ${SR_INCLUDE_DIR})
target_link_libraries(sr_codegen PUBLIC sr_ir sr_common)
