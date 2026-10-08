// RC-FP-003: end-to-end scalar FP64 tests.
//
// Three guest programs cover the double-precision pipeline:
//   1. Add:      integer -> SCVTF D -> FADD D -> printf("%.6f") -> 10.000000
//   2. Saturate: (2^63-1) * 2 as double -> FCVTZS X,D -> INT64_MAX
//   3. NaN:      0.0 / 0.0 -> FCVTZS X,D -> 0
//
// Translates ARM64 bytecode -> IR -> C, compiles with g++ (first with
// -fsanitize=undefined, retried plainly when the toolchain lacks the
// sanitizer runtime), runs, asserts the printed result line. Skipped only
// when g++ is not on PATH.
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

TEST(E2EFP64, AddDoubles) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // mov x0,#3 ; mov x1,#7 ; scvtf d0,x0 ; scvtf d1,x1 ;
    // fadd d0,d0,d1 ; ret
    const uint8_t code[] = {
        0x60, 0x00, 0x80, 0xD2,  // mov x0,#3   0xD2800060
        0xE1, 0x00, 0x80, 0xD2,  // mov x1,#7   0xD28000E1
        0x00, 0x00, 0x23, 0x9E,  // scvtf d0,x0 0x9E230000
        0x21, 0x00, 0x23, 0x9E,  // scvtf d1,x1 0x9E230021
        0x00, 0x28, 0x61, 0x1E,  // fadd d0,d0,d1 0x1E612800
        0xC0, 0x03, 0x5F, 0xD6,  // ret         0xD65F03C0
    };

    const std::string output =
        BuildRun(code, sizeof(code), "e2e_fp64_add",
                 "printf(\"result = %.6f\\n\", sr_v_get_d(&ctx.v[0]));");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_NE(output.find("result = 10.000000"), std::string::npos)
        << "expected 'result = 10.000000', got:\n" << output;

    double value = 0.0;
    ASSERT_EQ(std::sscanf(output.c_str(), "result = %lf", &value), 1)
        << "failed to parse numeric output: " << output;
    EXPECT_DOUBLE_EQ(value, 10.0);
}

TEST(E2EFP64, Saturate64Positive) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // x0 = 2^63-1 ; x1 = 2 ; d0 = (double)x0 ; d1 = (double)x1 ;
    // d0 = d0*d1 ; x2 = fcvtzs d0 ; x0 = x2 ; ret
    const uint8_t code[] = {
        0xE0, 0xFF, 0x9F, 0xD2,  // movz x0,#0xFFFF          0xD29FFFE0
        0xE0, 0xFF, 0xBF, 0xF2,  // movk x0,#0xFFFF,lsl16    0xF2BFFFE0
        0xE0, 0xFF, 0xDF, 0xF2,  // movk x0,#0xFFFF,lsl32    0xF2DFFFE0
        0xE0, 0xFF, 0xEF, 0xF2,  // movk x0,#0x7FFF,lsl48    0xF2EFFFE0
        0x41, 0x00, 0x80, 0xD2,  // mov x1,#2                0xD2800041
        0x00, 0x00, 0x23, 0x9E,  // scvtf d0,x0              0x9E230000
        0x21, 0x00, 0x23, 0x9E,  // scvtf d1,x1              0x9E230021
        0x00, 0x08, 0x61, 0x1E,  // fmul d0,d0,d1            0x1E610800
        0x02, 0x00, 0x79, 0x9E,  // fcvtzs x2,d0             0x9E790002
        0xE0, 0x03, 0x02, 0xAA,  // mov x0,x2                0xAA0203E0
        0xC0, 0x03, 0x5F, 0xD6,  // ret                      0xD65F03C0
    };

    const std::string output =
        BuildRun(code, sizeof(code), "e2e_fp64_sat64",
                 "printf(\"result = %lld\\n\", "
                 "(long long)(int64_t)ctx.x[0]);");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_NE(output.find("result = 9223372036854775807"), std::string::npos)
        << "expected 'result = 9223372036854775807', got:\n" << output;
}

TEST(E2EFP64, NaNConvertsToZero) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // mov x0,#0 ; mov x1,#0 ; scvtf d0,x0 ; scvtf d1,x1 ;
    // fdiv d0,d0,d1 ; fcvtzs x0,d0 ; ret
    const uint8_t code[] = {
        0x00, 0x00, 0x80, 0xD2,  // mov x0,#0   0xD2800000
        0x01, 0x00, 0x80, 0xD2,  // mov x1,#0   0xD2800001
        0x00, 0x00, 0x23, 0x9E,  // scvtf d0,x0 0x9E230000
        0x21, 0x00, 0x23, 0x9E,  // scvtf d1,x1 0x9E230021
        0x00, 0x18, 0x61, 0x1E,  // fdiv d0,d0,d1 0x1E611800
        0x00, 0x00, 0x79, 0x9E,  // fcvtzs x0,d0 0x9E790000
        0xC0, 0x03, 0x5F, 0xD6,  // ret         0xD65F03C0
    };

    const std::string output =
        BuildRun(code, sizeof(code), "e2e_fp64_nan",
                 "printf(\"result = %lld\\n\", "
                 "(long long)(int64_t)ctx.x[0]);");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_NE(output.find("result = 0"), std::string::npos)
        << "expected 'result = 0', got:\n" << output;
}
