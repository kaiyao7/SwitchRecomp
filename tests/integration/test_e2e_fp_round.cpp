// RC-FP-005: end-to-end FSQRT and FRINT round-to-integer tests.
//
// Two guest programs:
//   1. FSQRT:  MOV X0,#9 -> SCVTF S0 -> FSQRT S0 -> printf("%.6f") -> 3.000000
//   2. FRINT:  FMOV S0,#2.5 -> FRINTZ S1 / FRINTP S2 / FRINTM S3 / FRINTA S4
//              -> printf four values:
//                 s1=2.000000 s2=3.000000 s3=2.000000 s4=3.000000
//
// Translates ARM64 bytecode -> IR -> C, compiles with g++ (first with
// -fsanitize=undefined, retried plainly), runs, asserts the printed values.
// Skipped when a working g++ is not available on PATH.
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

// Translate |code|, append a main whose body prints via |printer| (a printf
// statement), compile with g++ plus UBSan and run. Returns program stdout
// ("" on pipeline failure).
std::string BuildRun(const uint8_t* code, size_t size,
                     const std::string& stem,
                     const std::string& printer) {
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
)";
    full += "    ";
    full += printer;
    full += R"(
    return 0;
}
)";

    {
        std::ofstream f(out_c, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return "";
        f << full;
    }

    const std::string sources =
        "\"" + out_c + "\" \"" + runtime_dir + "/runtime.cpp\" \"" +
        runtime_dir + "/memory_manager.cpp\" \"" +
        runtime_dir + "/svc_table.cpp\" \"" +
        runtime_dir + "/svc_thread.cpp\" \"" +
        runtime_dir + "/hle_service.cpp\" \"" +
        platform_dir + "/memory.cpp\" \"" +
        platform_dir + "/thread.cpp\"";
    auto compile = [&](const std::string& extra) {
        std::string c =
            "g++ -std=c++17 " + extra + " -I \"" + include_dir + "\" -I \"" +
            runtime_dir + "\" " + sources + " -o \"" + out_exe + "\" 2>&1";
        return std::system(c.c_str());
    };
    // Prefer UBSan; if the toolchain cannot provide it, retry plainly.
    if (compile("-fsanitize=undefined -fno-sanitize-recover=all") != 0 &&
        compile("") != 0) {
        return "";
    }

#ifdef _WIN32
    return RunCapture("\"" + out_exe + "\"");
#else
    return RunCapture("\"./" + out_exe + "\"");
#endif
}

}  // namespace

TEST(E2EFPRound, SqrtSingle) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }
    // mov x0,#9 ; scvtf s0,x0 ; fsqrt s0,s0 ; ret
    const uint8_t code[] = {
        0x20, 0x01, 0x80, 0xD2,  // mov x0,#9     0xD2800120
        0x00, 0x00, 0x22, 0x9E,  // scvtf s0,x0   0x9E220000
        0x00, 0xC0, 0x21, 0x1E,  // fsqrt s0,s0   0x1E21C000
        0xC0, 0x03, 0x5F, 0xD6,  // ret           0xD65F03C0
    };

    const std::string output =
        BuildRun(code, sizeof(code), "e2e_fp_round_sqrt",
                 "printf(\"result = %.6f\\n\", sr_v_get_s(&ctx.v[0]));");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_NE(output.find("result = 3.000000"), std::string::npos)
        << "expected 'result = 3.000000', got:\n" << output;

    double value = 0.0;
    ASSERT_EQ(std::sscanf(output.c_str(), "result = %lf", &value), 1)
        << "failed to parse numeric output: " << output;
    EXPECT_DOUBLE_EQ(value, 3.0);
}

TEST(E2EFPRound, FrintVariants) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }
    // fmov s0,#2.5 ; frintz s1,s0 ; frintp s2,s0 ; frintm s3,s0 ;
    // frinta s4,s0 ; ret
    const uint8_t code[] = {
        0x00, 0x90, 0x20, 0x1E,  // fmov s0,#2.5  0x1E209000
        0x01, 0xC0, 0x25, 0x1E,  // frintz s1,s0  0x1E25C001
        0x02, 0xC0, 0x24, 0x1E,  // frintp s2,s0  0x1E24C002
        0x03, 0x40, 0x25, 0x1E,  // frintm s3,s0  0x1E254003
        0x04, 0x40, 0x26, 0x1E,  // frinta s4,s0  0x1E264004
        0xC0, 0x03, 0x5F, 0xD6,  // ret           0xD65F03C0
    };

    const std::string output =
        BuildRun(code, sizeof(code), "e2e_fp_round_frint",
                 "printf(\"s1=%.6f s2=%.6f s3=%.6f s4=%.6f\\n\", "
                 "sr_v_get_s(&ctx.v[1]), sr_v_get_s(&ctx.v[2]), "
                 "sr_v_get_s(&ctx.v[3]), sr_v_get_s(&ctx.v[4]));");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    const std::string expected =
        "s1=2.000000 s2=3.000000 s3=2.000000 s4=3.000000";
    EXPECT_NE(output.find(expected), std::string::npos)
        << "expected '" << expected << "', got:\n" << output;

    double v1 = 0, v2 = 0, v3 = 0, v4 = 0;
    ASSERT_EQ(std::sscanf(output.c_str(),
                          "s1=%lf s2=%lf s3=%lf s4=%lf",
                          &v1, &v2, &v3, &v4), 4)
        << "failed to parse four numeric outputs: " << output;
    EXPECT_DOUBLE_EQ(v1, 2.0);  // FRINTZ
    EXPECT_DOUBLE_EQ(v2, 3.0);  // FRINTP
    EXPECT_DOUBLE_EQ(v3, 2.0);  // FRINTM
    EXPECT_DOUBLE_EQ(v4, 3.0);  // FRINTA
}
