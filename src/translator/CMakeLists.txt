add_library(sr_translator STATIC translator.cpp)
target_include_directories(sr_translator PUBLIC ${SR_INCLUDE_DIR})
target_link_libraries(sr_translator PUBLIC sr_ir sr_common)
