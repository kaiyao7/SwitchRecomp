// RC-FP-002: end-to-end FCMP/B.cond and FCVTZS/SCVTF tests.
// Test 1: fcmp s0,s1 (1.5 vs 2.5) ; b.lt around a fmov s2,#9.0 — asserts the
//         branch is taken and s2 keeps its pre-branch value 3.0.
// Test 2: fmov s0,#3.75 ; fcvtzs w0,s0 ; scvtf s0,w0 — asserts result 3.0.
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

// Translate |code|, append a main printing sr_v_get_s(&ctx.v[2]), compile and run.
// Returns the program stdout (empty string on a pipeline failure).
std::string BuildRun(const uint8_t* code, size_t size,
                     const std::string& stem) {
    const std::string root = ProjectRoot();
    const std::string include_dir = root + "/include";
    const std::string runtime_dir = root + "/src/runtime";
    const std::string platform_dir = root + "/src/platform";
    const std::string out_c = stem + ".c";
    const std::string out_exe = stem + ".exe";

    auto result = sr::translator::Translate(
        sr::common::span<const uint8_t>(code, size), {});
    if (!result) return "";

    auto c_src = sr::codegen::GenerateC(*result.value(), {});
    if (!c_src) return "";

    std::string full = c_src.value();
    full += R"(
#include <stdio.h>
#include <string.h>
int main() {
    GuestContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    fn_0(&ctx);
    printf("result = %.6f\n", sr_v_get_s(&ctx.v[2]));
    return 0;
}
)";

    {
        std::ofstream f(out_c, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return "";
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
    if (std::system(cmd.c_str()) != 0) return "";

#ifdef _WIN32
    return RunCapture("\"" + out_exe + "\"");
#else
    return RunCapture("\"./" + out_exe + "\"");
#endif
}

}  // namespace

TEST(E2E, FCmpBranchLessThan) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // Guest layout (4-byte aligned):
    //   0x00 fmov s0,#1.5   0x1E2F1000
    //   0x04 fmov s1,#2.5   0x1E209001
    //   0x08 fmov s2,#3.0   0x1E211002  (imm8=0x08; default result)
    //   0x0C fcmp s0,s1     0x1E212000
    //   0x10 b.lt +8        0x5400004B  (target 0x18: skip next word)
    //   0x14 fmov s2,#9.0   0x1E245002  (imm8=0x22; only reached when LT false)
    //   0x18 ret            0xD65F03C0
    // With 1.5 < 2.5 the branch must be taken, so 9.0 is skipped and s2
    // stays 3.0. If the branch failed, fallthrough would yield 9.0.
    const uint8_t code[] = {
        0x00, 0x10, 0x2F, 0x1E,
        0x01, 0x90, 0x20, 0x1E,
        0x02, 0x10, 0x21, 0x1E,
        0x00, 0x20, 0x21, 0x1E,
        0x4B, 0x00, 0x00, 0x54,
        0x02, 0x50, 0x24, 0x1E,
        0xC0, 0x03, 0x5F, 0xD6,
    };

    const std::string output = BuildRun(code, sizeof(code),
                                        "e2e_fcmp_output");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_NE(output.find("result = 3.000000"), std::string::npos)
        << "expected 'result = 3.000000' in output, got:\n" << output;

    float value = 0.0f;
    ASSERT_EQ(std::sscanf(output.c_str(), "result = %f", &value), 1)
        << "failed to parse numeric output: " << output;
    EXPECT_FLOAT_EQ(value, 3.0f);
}

TEST(E2E, FConvertTruncateAndBack) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // Guest layout:
    //   0x00 fmov s0,#3.75  0x1E21D000  (imm8=0x0E -> 0x40700000)
    //   0x04 fcvtzs w0,s0   0x1E780000  (3.75 -> 3, toward zero)
    //   0x08 scvtf s0,w0    0x1E220000  (3 -> 3.0)
    //   0x0C ret            0xD65F03C0
    // The result stays in s0 (BuildRun prints f[2], so this test builds
    // and runs the pipeline itself).
    const std::string root = ProjectRoot();
    const std::string include_dir = root + "/include";
    const std::string runtime_dir = root + "/src/runtime";
    const std::string platform_dir = root + "/src/platform";
    const std::string out_c = "e2e_fcvt_output.c";
    const std::string out_exe = "e2e_fcvt_output.exe";

    const uint8_t code[] = {
        0x00, 0xD0, 0x21, 0x1E,
        0x00, 0x00, 0x78, 0x1E,
        0x00, 0x00, 0x22, 0x1E,
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
        ASSERT_TRUE(f.is_open());
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
    const std::string output = RunCapture("\"" + out_exe + "\"");
#else
    const std::string output = RunCapture("\"./" + out_exe + "\"");
#endif
    ASSERT_FALSE(output.empty()) << "compiled binary produced no output";
    EXPECT_NE(output.find("result = 3.000000"), std::string::npos)
        << "expected 'result = 3.000000' in output, got:\n" << output;

    float value = 0.0f;
    ASSERT_EQ(std::sscanf(output.c_str(), "result = %f", &value), 1)
        << "failed to parse numeric output: " << output;
    EXPECT_FLOAT_EQ(value, 3.0f);
}
