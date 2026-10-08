// SIMD stage 2 end-to-end tests:
//   1) MOVI Vd.4S,#fp + FADD Vd.4S -> all four result lanes equal 4.0
//   2) FMOV Sd,#imm + FDUP Vd.4S + INS lane + UMOV lane -> lane pattern
//      (3,3,7,3) and the extracted f32 bit pattern landing in a GPR (w3).
// Translates ARM64 bytecode -> IR -> C, compiles the generated C with g++,
// runs the binary, and asserts on its printed output. Skipped when a working
// g++ compiler is not available on PATH.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

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

// Translate |words| -> IR -> C, append |main_src|, write the result to
// e2e_simd<tag>_output.c and compile it to e2e_simd<tag>_output.exe with g++.
// On success returns true and stores the full generated source in
// |full_src_out| for diagnostic output.
bool BuildProgram(const std::vector<uint32_t>& words,
                  const std::string& main_src,
                  const std::string& tag,
                  std::string* full_src_out) {
    std::vector<uint8_t> code;
    code.reserve(words.size() * 4);
    for (uint32_t w : words) {
        code.push_back(static_cast<uint8_t>(w & 0xFFu));
        code.push_back(static_cast<uint8_t>((w >> 8) & 0xFFu));
        code.push_back(static_cast<uint8_t>((w >> 16) & 0xFFu));
        code.push_back(static_cast<uint8_t>((w >> 24) & 0xFFu));
    }

    auto result = sr::translator::Translate(
        sr::common::span<const uint8_t>(code.data(), code.size()), {});
    if (!result) {
        ADD_FAILURE() << "translate failed: " << result.error().message;
        return false;
    }

    auto c_src = sr::codegen::GenerateC(*result.value(), {});
    if (!c_src) {
        ADD_FAILURE() << "codegen failed: " << c_src.error().message;
        return false;
    }

    const std::string root = ProjectRoot();
    const std::string include_dir = root + "/include";
    const std::string runtime_dir = root + "/src/runtime";
    const std::string platform_dir = root + "/src/platform";
    const std::string out_c = "e2e_simd" + tag + "_output.c";
    const std::string out_exe = "e2e_simd" + tag + "_output.exe";

    std::string full = c_src.value() + main_src;
    *full_src_out = full;

    {
        std::ofstream f(out_c, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            ADD_FAILURE() << "failed to open " << out_c;
            return false;
        }
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
    return std::system(cmd.c_str()) == 0;
}

// Run e2e_simd<tag>_output.exe and return its captured stdout.
std::string RunProgram(const std::string& tag) {
    const std::string out_exe = "e2e_simd" + tag + "_output.exe";
#ifdef _WIN32
    return RunCapture("\"" + out_exe + "\"");
#else
    return RunCapture("\"./" + out_exe + "\"");
#endif
}

// Parse the float after "laneN = " from |output| into |value|.
bool ParseLane(const std::string& output, int lane, float* value) {
    const std::string key = "lane" + std::to_string(lane) + " = ";
    const std::size_t pos = output.find(key);
    if (pos == std::string::npos) return false;
    return std::sscanf(output.c_str() + pos + key.size(), "%f", value) == 1;
}

}  // namespace

TEST(E2E, SimdMoviFadd4S) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // Encodings (keystone+capstone verified):
    //   movi v0.4s,#1.5 = 0x4F03F700 (imm8=0x78 -> 0x3FC00000)
    //   movi v1.4s,#2.5 = 0x4F00F481 (imm8=0x04 -> 0x40200000)
    //   fadd v2.4s,v0,v1 = 0x4E21D402
    //   ret = 0xD65F03C0
    const std::vector<uint32_t> words = {
        0x4F03F700u, 0x4F00F481u, 0x4E21D402u, 0xD65F03C0u,
    };

    const std::string main_src = R"(
#include <stdio.h>
#include <string.h>
int main(void) {
    GuestContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    fn_0(&ctx);
    for (int i = 0; i < 4; ++i)
        printf("lane%d = %.6f\n", i, sr_v_get_lane_s(&ctx.v[2], i));
    return 0;
}
)";

    std::string full;
    ASSERT_TRUE(BuildProgram(words, main_src, "1", &full))
        << "g++ compile failed for generated C:\n" << full;

    const std::string output = RunProgram("1");
    ASSERT_FALSE(output.empty()) << "compiled binary produced no output";

    for (int i = 0; i < 4; ++i) {
        const std::string expected =
            "lane" + std::to_string(i) + " = 4.000000";
        EXPECT_NE(output.find(expected), std::string::npos)
            << "missing '" << expected << "' in output:\n" << output;
        float value = 0.0f;
        ASSERT_TRUE(ParseLane(output, i, &value))
            << "failed to parse lane" << i << " output: " << output;
        EXPECT_FLOAT_EQ(value, 4.0f);
    }
}

TEST(E2E, SimdFdupInsUmov) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // Encodings (keystone+capstone verified):
    //   fmov s0,#3.0       = 0x1E211000
    //   fdup v1.4s,s0      = 0x4E040401 (keystone mnemonic: dup)
    //   fmov s2,#7.0       = 0x1E239002
    //   ins v1.s[2],s2     = 0x6E140441 (capstone alias: mov)
    //   umov w3,v1.s[2]    = 0x0E143C23 (capstone alias: mov)
    //   ret                = 0xD65F03C0
    const std::vector<uint32_t> words = {
        0x1E211000u, 0x4E040401u, 0x1E239002u,
        0x6E140441u, 0x0E143C23u, 0xD65F03C0u,
    };

    const std::string main_src = R"(
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int main(void) {
    GuestContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    fn_0(&ctx);
    for (int i = 0; i < 4; ++i)
        printf("lane%d = %.6f\n", i, sr_v_get_lane_s(&ctx.v[1], i));
    union { uint32_t u; float f; } cvt;
    cvt.u = (uint32_t)ctx.x[3];
    printf("w3 = 0x%08X\n", (unsigned)cvt.u);
    printf("w3f = %.6f\n", cvt.f);
    return 0;
}
)";

    std::string full;
    ASSERT_TRUE(BuildProgram(words, main_src, "2", &full))
        << "g++ compile failed for generated C:\n" << full;

    const std::string output = RunProgram("2");
    ASSERT_FALSE(output.empty()) << "compiled binary produced no output";

    const float expected_lanes[4] = {3.0f, 3.0f, 7.0f, 3.0f};
    for (int i = 0; i < 4; ++i) {
        char lit[40];
        std::snprintf(lit, sizeof(lit), "lane%d = %.6f", i,
                      static_cast<double>(expected_lanes[i]));
        EXPECT_NE(output.find(lit), std::string::npos)
            << "missing '" << lit << "' in output:\n" << output;
        float value = 0.0f;
        ASSERT_TRUE(ParseLane(output, i, &value))
            << "failed to parse lane" << i << " output: " << output;
        EXPECT_FLOAT_EQ(value, expected_lanes[i]);
    }

    // UMOV copies the raw f32 bit pattern (7.0 = 0x40E00000) into w3.
    EXPECT_NE(output.find("w3 = 0x40E00000"), std::string::npos)
        << "expected 'w3 = 0x40E00000' in output:\n" << output;
    EXPECT_NE(output.find("w3f = 7.000000"), std::string::npos)
        << "expected 'w3f = 7.000000' in output:\n" << output;
}
