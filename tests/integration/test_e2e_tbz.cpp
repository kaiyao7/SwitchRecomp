// TBZ / TBNZ end-to-end execution tests.
//
// Every program is six guest words:
//   word 0: movz x1,#<value>
//   word 1: tbz|tbnz x1,#<bit>,+12   target word 4
//   word 2: movz x0,#8               fallthrough path
//   word 3: b +8                     target word 5 (skip the taken path)
//   word 4: movz x0,#7               taken path
//   word 5: ret
//
// The tested bit is clear for x1 == 1 and set for x1 == 4, so the same layout
// exercises both the branch-taken and the fallthrough direction of each form.
// The upper-bit case loads 1 << 48 to prove that b5 (encoding bit 31) selects
// the 64-bit register width.
//
// Generated programs are compiled with g++ using a free -fsanitize=undefined
// attempt first, then a plain compile when the sanitizer runtime is missing.
// Skipped entirely when g++ is not on PATH.
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

// Translate |code|, append a main printing (int64_t)ctx.x[0], compile with g++
// and run. Returns program stdout ("" on pipeline failure).
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

bool ExpectResult(const std::string& output, long long expected) {
    const std::string needle = "result = " + std::to_string(expected);
    return output.find(needle) != std::string::npos;
}

}  // namespace

TEST(E2ETestBit, TbzBitClearBranches) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // x1 = 1: bit 2 clear -> taken -> x0 = 7.
    const uint8_t code[] = {
        0x21, 0x00, 0x80, 0xD2,  // movz x1,#1    0xD2800021
        0x61, 0x00, 0x10, 0x36,  // tbz x1,#2,+12 0x36100061
        0x00, 0x01, 0x80, 0xD2,  // movz x0,#8    0xD2800100
        0x02, 0x00, 0x00, 0x14,  // b +8          0x14000002
        0xE0, 0x00, 0x80, 0xD2,  // movz x0,#7    0xD28000E0
        0xC0, 0x03, 0x5F, 0xD6,  // ret           0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code), "e2e_tbz_clear");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 7)) << "expected 'result = 7', got:\n"
                                        << output;
}

TEST(E2ETestBit, TbzBitSetFallsThrough) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // x1 = 4: bit 2 set -> not taken -> x0 = 8.
    const uint8_t code[] = {
        0x81, 0x00, 0x80, 0xD2,  // movz x1,#4    0xD2800081
        0x61, 0x00, 0x10, 0x36,  // tbz x1,#2,+12 0x36100061
        0x00, 0x01, 0x80, 0xD2,  // movz x0,#8    0xD2800100
        0x02, 0x00, 0x00, 0x14,  // b +8          0x14000002
        0xE0, 0x00, 0x80, 0xD2,  // movz x0,#7    0xD28000E0
        0xC0, 0x03, 0x5F, 0xD6,  // ret           0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code), "e2e_tbz_set");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 8)) << "expected 'result = 8', got:\n"
                                        << output;
}

TEST(E2ETestBit, TbnzBitSetBranches) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // x1 = 4: bit 2 set -> taken -> x0 = 7.
    const uint8_t code[] = {
        0x81, 0x00, 0x80, 0xD2,  // movz x1,#4    0xD2800081
        0x61, 0x00, 0x10, 0x37,  // tbnz x1,#2,+12 0x37100061
        0x00, 0x01, 0x80, 0xD2,  // movz x0,#8    0xD2800100
        0x02, 0x00, 0x00, 0x14,  // b +8          0x14000002
        0xE0, 0x00, 0x80, 0xD2,  // movz x0,#7    0xD28000E0
        0xC0, 0x03, 0x5F, 0xD6,  // ret           0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code), "e2e_tbnz_set");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 7)) << "expected 'result = 7', got:\n"
                                        << output;
}

TEST(E2ETestBit, TbnzUpperBitUsesXRegister) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // x1 = 1 << 48; tbnz x1,#48 -> b5 = 1, b40 = 16 -> taken -> x0 = 7.
    const uint8_t code[] = {
        0x21, 0x00, 0xE0, 0xD2,  // movz x1,#1,lsl#48 0xD2E00021
        0x61, 0x00, 0x80, 0xB7,  // tbnz x1,#48,+12   0xB7800061
        0x00, 0x01, 0x80, 0xD2,  // movz x0,#8        0xD2800100
        0x02, 0x00, 0x00, 0x14,  // b +8              0x14000002
        0xE0, 0x00, 0x80, 0xD2,  // movz x0,#7        0xD28000E0
        0xC0, 0x03, 0x5F, 0xD6,  // ret               0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code), "e2e_tbnz_high");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 7)) << "expected 'result = 7', got:\n"
                                        << output;
}
