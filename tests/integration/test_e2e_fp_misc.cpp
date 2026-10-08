// RC-FP-004: end-to-end scalar FP misc tests (FNEG/FABS/FCSEL/FCVT).
//
// Three guest programs:
//   1. FABS:  MOVN #2 -> SCVTF D -> FABS D -> printf("%.6f") -> 3.000000
//   2. FCSEL: 1.0/2.0 -> FCMP -> FCSEL S,LT -> printf("%.6f") -> 2.000000
//   3. FCVT:  #5 -> SCVTF D -> FCVT S -> printf("%.6f") -> 5.000000
//
// Translates ARM64 bytecode -> IR -> C, compiles with g++ (first with
// -fsanitize=undefined, retried plainly), runs, asserts the printed line.
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

TEST(E2EFPMisc, AbsDouble) {
    // movn x0,#2 ; scvtf d0,w0 ; fabs d0,d0 ; ret
    const uint8_t code[] = {
        0x40, 0x00, 0x80, 0x12,  // movn x0,#2    0x12800040
        0x00, 0x00, 0x23, 0x1E,  // scvtf d0,w0   0x1E230000
        0x00, 0xC0, 0x60, 0x1E,  // fabs d0,d0    0x1E60C000
        0xC0, 0x03, 0x5F, 0xD6,  // ret           0xD65F03C0
    };

    const std::string output =
        BuildRun(code, sizeof(code), "e2e_fp_misc_abs",
                 "printf(\"result = %.6f\\n\", sr_v_get_d(&ctx.v[0]));");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_NE(output.find("result = 3.000000"), std::string::npos)
        << "expected 'result = 3.000000', got:\n" << output;

    double value = 0.0;
    ASSERT_EQ(std::sscanf(output.c_str(), "result = %lf", &value), 1)
        << "failed to parse numeric output: " << output;
    EXPECT_DOUBLE_EQ(value, 3.0);
}

TEST(E2EFPMisc, ConditionalSelect) {
    // fmov s0,#1.0 ; fmov s1,#2.0 ; fcmp s1,s0 ; fcsel s2,s0,s1,lt ; ret
    // 2.0 < 1.0 is false, so FCSEL takes Rm = s1 = 2.0.
    const uint8_t code[] = {
        0x00, 0x10, 0x2E, 0x1E,  // fmov s0,#1.0  0x1E2E1000
        0x01, 0x10, 0x20, 0x1E,  // fmov s1,#2.0  0x1E201001
        0x20, 0x20, 0x20, 0x1E,  // fcmp s1,s0    0x1E202020
        0x02, 0xBC, 0x21, 0x1E,  // fcsel s2,s0,s1,lt 0x1E21BC02
        0xC0, 0x03, 0x5F, 0xD6,  // ret           0xD65F03C0
    };

    const std::string output =
        BuildRun(code, sizeof(code), "e2e_fp_misc_fcsel",
                 "printf(\"result = %.6f\\n\", sr_v_get_s(&ctx.v[2]));");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_NE(output.find("result = 2.000000"), std::string::npos)
        << "expected 'result = 2.000000', got:\n" << output;

    double value = 0.0;
    ASSERT_EQ(std::sscanf(output.c_str(), "result = %lf", &value), 1)
        << "failed to parse numeric output: " << output;
    EXPECT_DOUBLE_EQ(value, 2.0);
}

TEST(E2EFPMisc, ConvertDoubleToSingle) {
    // mov x0,#5 ; scvtf d0,x0 ; fcvt s0,d0 ; ret
    const uint8_t code[] = {
        0xA0, 0x00, 0x80, 0xD2,  // mov x0,#5     0xD28000A0
        0x00, 0x00, 0x23, 0x9E,  // scvtf d0,x0   0x9E230000
        0x00, 0x40, 0x62, 0x1E,  // fcvt s0,d0    0x1E624000
        0xC0, 0x03, 0x5F, 0xD6,  // ret           0xD65F03C0
    };

    const std::string output =
        BuildRun(code, sizeof(code), "e2e_fp_misc_fcvt",
                 "printf(\"result = %.6f\\n\", sr_v_get_s(&ctx.v[0]));");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_NE(output.find("result = 5.000000"), std::string::npos)
        << "expected 'result = 5.000000', got:\n" << output;

    double value = 0.0;
    ASSERT_EQ(std::sscanf(output.c_str(), "result = %lf", &value), 1)
        << "failed to parse numeric output: " << output;
    EXPECT_DOUBLE_EQ(value, 5.0);
}
