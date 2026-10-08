add_executable(recomp_cli main.cpp)
target_link_libraries(recomp_cli PRIVATE sr_translator sr_codegen sr_ir sr_common)
target_include_directories(recomp_cli PRIVATE ${SR_INCLUDE_DIR})
