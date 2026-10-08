// RC-FP-001: end-to-end scalar FP32 add test.
// Translates ARM64 bytecode (fmov s0,#1.5 ; fmov s1,#2.5 ; fadd s0,s0,s1 ; ret)
// -> IR -> C, compiles with g++, runs, asserts sr_v_get_s(&ctx.v[0]) == 4.0.
// Skipped when a working g++ compiler is not available on PATH.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "switchrecomp/codegen/codegen.h"
#include "switchrecomp/common/span.h"
#include "switchrecomp/translator/translator.h"

#ifndef SR_INCLUDE_DIR
#error "SR_INCLUDE_DIR must be defined via CMake"
#endif

#ifdef _WIN32
#define SR_POPEN _popen
#define SR_PCLOSE _pclose
#else
#define SR_POPEN popen
#define SR_PCLOSE pclose
#endif

namespace {

std::string ProjectRoot() {
    std::string inc = SR_INCLUDE_DIR;
    const std::string sep = "/include";
    const std::string sep_win = "\\include";
    if (inc.size() >= sep.size() &&
        inc.compare(inc.size() - sep.size(), sep.size(), sep) == 0) {
        return inc.substr(0, inc.size() - sep.size());
    }
    if (inc.size() >= sep_win.size() &&
        inc.compare(inc.size() - sep_win.size(), sep_win.size(), sep_win) == 0) {
        return inc.substr(0, inc.size() - sep_win.size());
    }
    return inc;
}

std::string RunCapture(const std::string& cmd) {
    std::string output;
    FILE* pipe = SR_POPEN(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    SR_PCLOSE(pipe);
    return output;
}

bool HasGxx() {
#ifdef _WIN32
    const char* null_dev = "nul";
#else
    const char* null_dev = "/dev/null";
#endif
    std::string cmd = std::string("g++ --version > ") + null_dev + " 2>&1";
    return std::system(cmd.c_str()) == 0;
}

}  // namespace

TEST(E2E, FloatAddScalars) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    const std::string root = ProjectRoot();
    const std::string include_dir = root + "/include";
    const std::string runtime_dir = root + "/src/runtime";
    const std::string platform_dir = root + "/src/platform";
    const std::string out_c = "e2e_float_output.c";
    const std::string out_exe = "e2e_float_output.exe";

    // Encodings (verified against ARM ARM VFPExpandImm):
    //   fmov s0,#1.5 = 0x1E2F1000 (imm8=0x78 -> 0x3FC00000)
    //   fmov s1,#2.5 = 0x1E209001 (imm8=0x04 -> 0x40200000)
    //   fadd s0,s0,s1 = 0x1E212800
    //   ret = 0xD65F03C0
    const uint8_t code[] = {
        0x00, 0x10, 0x2F, 0x1E,
        0x01, 0x90, 0x20, 0x1E,
        0x00, 0x28, 0x21, 0x1E,
        0xC0, 0x03, 0x5F, 0xD6,
    };

    auto result = sr::translator::Translate(
        sr::common::span<const uint8_t>(code, sizeof(code)), {});
    ASSERT_TRUE(result) << "translate failed: " << result.error().message;

    auto c_src = sr::codegen::GenerateC(*result.value(), {});
    ASSERT_TRUE(c_src) << "codegen failed: " << c_src.error().message;

    std::string full = c_src.value();
    full += R"(
#include <stdio.h>
#include <string.h>
int main() {
    GuestContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    fn_0(&ctx);
    printf("result = %.6f\n", sr_v_get_s(&ctx.v[0]));
    return 0;
}
)";

    {
        std::ofstream f(out_c, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.is_open()) << "failed to open " << out_c;
        f << full;
    }

    std::string cmd = "g++ -std=c++17 -I \"" + include_dir + "\" -I \"" +
                      runtime_dir + "\" \"" + out_c + "\" \"" +
                      runtime_dir + "/runtime.cpp\" \"" +
                      runtime_dir + "/memory_manager.cpp\" \"" +
                      runtime_dir + "/svc_table.cpp\" \"" +
                      runtime_dir + "/svc_thread.cpp\" \"" +
                      runtime_dir + "/hle_service.cpp\" \"" +
                      platform_dir + "/memory.cpp\" \"" +
                      platform_dir + "/thread.cpp\" -o \"" + out_exe + "\" 2>&1";
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "g++ compile failed for generated C:\n" << full;

#ifdef _WIN32
    std::string run_cmd = "\"" + out_exe + "\"";
#else
    std::string run_cmd = "\"./" + out_exe + "\"";
#endif
    std::string output = RunCapture(run_cmd);
    ASSERT_FALSE(output.empty()) << "compiled binary produced no output";

    EXPECT_NE(output.find("result = 4.000000"), std::string::npos)
        << "expected 'result = 4.000000' in output, got:\n" << output;

    // Numeric assertion with float tolerance.
    float value = 0.0f;
    ASSERT_EQ(std::sscanf(output.c_str(), "result = %f", &value), 1)
        << "failed to parse numeric output: " << output;
    EXPECT_FLOAT_EQ(value, 4.0f);
}
