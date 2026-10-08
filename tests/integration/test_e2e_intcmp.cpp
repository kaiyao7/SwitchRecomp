// RC-FIX-002: end-to-end integer CMP -> NZCV -> B.cond, plus FCVTZS
// saturation execution tests.
//
// Integer cases (8 guest words each; all words emit exactly one IR
// instruction, so guest word index == IR ordinal):
//   word 0: mov x0,#A
//   word 1: mov x1,#B
//   word 2: cmp x0,x1
//   word 3: b.<cc> +12        target word 6 (taken path)
//   word 4: mov x0,#8         not-taken path
//   word 5: b +8              target word 7 (skip taken path)
//   word 6: mov x0,#7         taken path
//   word 7: ret
//
// Saturation cases: fmov s0,#2.0; five fmul s0,s0,s0 -> 2^32; fcvtzs w0,s0
// (-> INT32_MAX). The negative case additionally loads s1 = #-1.0 and
// multiplies once (-> -2^32) before fcvtzs (-> INT32_MIN).
//
// Generated programs are first compiled with -fsanitize=undefined (so any
// remaining UB in the clamp/compare lowering is reported); when the toolchain
// lacks the sanitizer runtime (e.g. MinGW without libubsan) the test retries
// with a plain compile. Skipped entirely when g++ is not on PATH.
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

// Translate |code|, append a main printing (int64_t)ctx.x[0], compile with
// g++ plus UBSan and run. Returns program stdout ("" on pipeline failure).
// On failure the compiler's stderr is included so ASSERT failures show it.
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
    printf("result = %lld\n", (long long)(int64_t)ctx.x[0]);
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

// Expect the exact "result = <expected>" line and return true on match.
bool ExpectResult(const std::string& output, long long expected) {
    const std::string needle =
        "result = " + std::to_string(expected);
    return output.find(needle) != std::string::npos;
}

}  // namespace

TEST(E2EIntCmp, LessThanTaken) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // 1 < 2 -> LT taken -> word 6 -> x0 = 7.
    const uint8_t code[] = {
        0x20, 0x00, 0x80, 0xD2,  // mov x0,#1   0xD2800020
        0x41, 0x00, 0x80, 0xD2,  // mov x1,#2   0xD2800041
        0x1F, 0x00, 0x01, 0xEB,  // cmp x0,x1   0xEB01001F
        0x6B, 0x00, 0x00, 0x54,  // b.lt +12    0x5400006B
        0x00, 0x01, 0x80, 0xD2,  // mov x0,#8   0xD2800100
        0x02, 0x00, 0x00, 0x14,  // b +8        0x14000002
        0xE0, 0x00, 0x80, 0xD2,  // mov x0,#7   0xD28000E0
        0xC0, 0x03, 0x5F, 0xD6,  // ret         0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code),
                                        "e2e_intcmp_lt_taken");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 7))
        << "expected 'result = 7', got:\n" << output;
}

TEST(E2EIntCmp, GreaterEqualTaken) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // 5 >= 2 -> GE taken -> word 6 -> x0 = 7.
    const uint8_t code[] = {
        0xA0, 0x00, 0x80, 0xD2,  // mov x0,#5   0xD28000A0
        0x41, 0x00, 0x80, 0xD2,  // mov x1,#2   0xD2800041
        0x1F, 0x00, 0x01, 0xEB,  // cmp x0,x1   0xEB01001F
        0x6A, 0x00, 0x00, 0x54,  // b.ge +12    0x5400006A
        0x00, 0x01, 0x80, 0xD2,  // mov x0,#8   0xD2800100
        0x02, 0x00, 0x00, 0x14,  // b +8        0x14000002
        0xE0, 0x00, 0x80, 0xD2,  // mov x0,#7   0xD28000E0
        0xC0, 0x03, 0x5F, 0xD6,  // ret         0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code),
                                        "e2e_intcmp_ge_taken");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 7))
        << "expected 'result = 7', got:\n" << output;
}

TEST(E2EIntCmp, LessThanNotTaken) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // 5 < 2 false -> fallthrough -> x0 = 8; the word-6 path is skipped by
    // the unconditional b at word 5.
    const uint8_t code[] = {
        0xA0, 0x00, 0x80, 0xD2,  // mov x0,#5   0xD28000A0
        0x41, 0x00, 0x80, 0xD2,  // mov x1,#2   0xD2800041
        0x1F, 0x00, 0x01, 0xEB,  // cmp x0,x1   0xEB01001F
        0x6B, 0x00, 0x00, 0x54,  // b.lt +12    0x5400006B
        0x00, 0x01, 0x80, 0xD2,  // mov x0,#8   0xD2800100
        0x02, 0x00, 0x00, 0x14,  // b +8        0x14000002
        0xE0, 0x00, 0x80, 0xD2,  // mov x0,#7   0xD28000E0
        0xC0, 0x03, 0x5F, 0xD6,  // ret         0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code),
                                        "e2e_intcmp_lt_not_taken");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 8))
        << "expected 'result = 8', got:\n" << output;
}

TEST(E2EFcvtzs, SaturatePositive32) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // s0 = 2.0; 5 squares -> 2^32 = 4294967296.0; fcvtzs w0,s0 must
    // saturate to INT32_MAX.
    const uint8_t code[] = {
        0x00, 0x10, 0x20, 0x1E,  // fmov s0,#2.0       0x1E201000
        0x00, 0x08, 0x20, 0x1E,  // fmul s0,s0,s0      0x1E200800
        0x00, 0x08, 0x20, 0x1E,
        0x00, 0x08, 0x20, 0x1E,
        0x00, 0x08, 0x20, 0x1E,
        0x00, 0x08, 0x20, 0x1E,
        0x00, 0x00, 0x78, 0x1E,  // fcvtzs w0,s0       0x1E780000
        0xC0, 0x03, 0x5F, 0xD6,  // ret                0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code),
                                        "e2e_fcvtzs_sat_pos");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 2147483647))
        << "expected 'result = 2147483647', got:\n" << output;
}

TEST(E2EFcvtzs, SaturateNegative32) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // s0 = 2^32 (as above); s1 = -1.0; s0 = s0 * s1 -> -2^32; fcvtzs must
    // saturate to INT32_MIN (read back signed from ctx.x[0]).
    const uint8_t code[] = {
        0x00, 0x10, 0x20, 0x1E,  // fmov s0,#2.0       0x1E201000
        0x00, 0x08, 0x20, 0x1E,  // fmul s0,s0,s0 x5   0x1E200800
        0x00, 0x08, 0x20, 0x1E,
        0x00, 0x08, 0x20, 0x1E,
        0x00, 0x08, 0x20, 0x1E,
        0x00, 0x08, 0x20, 0x1E,
        0x01, 0x10, 0x3E, 0x1E,  // fmov s1,#-1.0      0x1E3E1001
        0x00, 0x08, 0x21, 0x1E,  // fmul s0,s0,s1      0x1E210800
        0x00, 0x00, 0x78, 0x1E,  // fcvtzs w0,s0       0x1E780000
        0xC0, 0x03, 0x5F, 0xD6,  // ret                0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code),
                                        "e2e_fcvtzs_sat_neg");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, -2147483648LL))
        << "expected 'result = -2147483648', got:\n" << output;
}
