// End-to-end feasibility proof for the numeric substrate in *generated code*.
//
// numeric.h defines bit-exact / UB-free semantics; numeric_emit.h turns them
// into host C++ expressions. This test emits a complete program from those
// helpers, compiles it with a real compiler at -O2, runs it, and checks the
// results on inputs that are undefined behaviour under naive C (shift >= width,
// 32-bit carry, NaN / out-of-range float->int, W high-bit garbage, arithmetic
// shift of a negative value, condition code after signed overflow).
//
// It is skipped when no g++ is on PATH (the MSVC-only CI job), matching the
// existing end-to-end test convention.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "switchrecomp/common/numeric_emit.h"

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
        inc.compare(inc.size() - sep.size(), sep.size(), sep) == 0)
        return inc.substr(0, inc.size() - sep.size());
    if (inc.size() >= sep_win.size() &&
        inc.compare(inc.size() - sep_win.size(), sep_win.size(), sep_win) == 0)
        return inc.substr(0, inc.size() - sep_win.size());
    return inc;
}

std::string RunCapture(const std::string& cmd) {
    std::string output;
    FILE* pipe = SR_POPEN(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) output += buf;
    SR_PCLOSE(pipe);
    return output;
}

bool HasGxx() {
#ifdef _WIN32
    const char* null_dev = "nul";
#else
    const char* null_dev = "/dev/null";
#endif
    return std::system((std::string("g++ --version > ") + null_dev +
                        " 2>&1").c_str()) == 0;
}

bool Has(const std::string& output, const std::string& kv) {
    return output.find(kv) != std::string::npos;
}

} // namespace

TEST(NumericCodegen, EmittedSemanticsSurviveHostOptimization) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping emitted-code test";
    }

    const std::string root = ProjectRoot();
    const std::string include_dir = root + "/include";
    const std::string out_c = "numeric_emit_test_output.cpp";
    const std::string out_exe = "numeric_emit_test_output.exe";

    const std::string program = sr::numeric::emit::SelfTestProgram();
    {
        std::ofstream f(out_c, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.is_open()) << "failed to open " << out_c;
        f << program;
    }

    // -O2 is the whole point: the host optimizer is allowed to exploit UB in
    // naive expressions. The emitted numeric helpers must remain correct.
    const std::string cmd = "g++ -std=c++17 -O2 -I \"" + include_dir +
                            "\" \"" + out_c + "\" -o \"" + out_exe +
                            "\" 2>&1";
    const int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "compile failed for emitted program:\n" << program;

#ifdef _WIN32
    const std::string run_cmd = "\"" + out_exe + "\"";
#else
    const std::string run_cmd = "\"./" + out_exe + "\"";
#endif
    const std::string output = RunCapture(run_cmd);
    ASSERT_FALSE(output.empty()) << "emitted program produced no output";

    EXPECT_TRUE(Has(output, "zext=1")) << output;
    EXPECT_TRUE(Has(output, "shl64=1")) << output;    // amount 64 -> 0
    EXPECT_TRUE(Has(output, "shl65=2")) << output;    // amount 65 -> 1
    EXPECT_TRUE(Has(output, "add=0 c=1")) << output;  // 32-bit carry out
    EXPECT_TRUE(Has(output, "cvt_nan=0")) << output;  // NaN -> 0
    EXPECT_TRUE(Has(output, "cvt_sat=9223372036854775807")) << output;
    EXPECT_TRUE(Has(output, "sar=f8000000")) << output;
    // 0x7FFFFFFF + 1 -> N=1, V=1, Z=0; GT = Z==0 && N==V -> true.
    EXPECT_TRUE(Has(output, "gt=1")) << output;
}
