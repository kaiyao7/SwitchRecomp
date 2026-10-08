// RC-PIPE-004: end-to-end integration test.
// Translates ARM64 bytecode -> IR -> C, compiles with g++, runs, asserts x0==1.
// Skipped when a working g++ compiler is not available on PATH (e.g. the
// Windows MSVC CI job, which has no gcc toolchain).
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

// Cross-platform pipe helpers: _popen/_pclose on Windows, popen/pclose elsewhere.
#ifdef _WIN32
#define SR_POPEN _popen
#define SR_PCLOSE _pclose
#else
#define SR_POPEN popen
#define SR_PCLOSE pclose
#endif

namespace {

// Strip the trailing "/include" (or "\include") from SR_INCLUDE_DIR to get the
// project root.
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

// Run a shell command and capture its stdout.
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

// Returns true if a C++ compiler (g++) is available on PATH.
bool HasGxx() {
    // g++ --version returns 0 when the compiler is present and working.
    // Use the platform-appropriate null device for output redirection.
#ifdef _WIN32
    const char* null_dev = "nul";
#else
    const char* null_dev = "/dev/null";
#endif
    std::string cmd = std::string("g++ --version > ") + null_dev + " 2>&1";
    int rc = std::system(cmd.c_str());
    return rc == 0;
}

}  // namespace

TEST(E2E, AddImmIncrementsX0) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    const std::string root = ProjectRoot();
    const std::string include_dir = root + "/include";
    const std::string runtime_dir = root + "/src/runtime";
    const std::string platform_dir = root + "/src/platform";
    const std::string out_c = "e2e_recomp_output.c";
    const std::string out_exe = "e2e_recomp_output.exe";

    // add x0,x0,#1 (0x91000400) ; ret (0xD65F03C0)
    const uint8_t code[] = {0x00, 0x04, 0x00, 0x91, 0xC0, 0x03, 0x5F, 0xD6};

    auto result = sr::translator::Translate(
        sr::common::span<const uint8_t>(code, sizeof(code)), {});
    ASSERT_TRUE(result) << "translate failed: " << result.error().message;

    auto c_src = sr::codegen::GenerateC(*result.value(), {});
    ASSERT_TRUE(c_src) << "codegen failed: " << c_src.error().message;

    // Append a host main() that zeroes context, calls fn_0, prints x0.
    std::string full = c_src.value();
    full += R"(
#include <stdio.h>
int main() {
    GuestContext ctx;
    for (int i = 0; i < 31; i++) ctx.x[i] = 0;
    ctx.sp = 0; ctx.pc = 0; ctx.nzcv = 0;
    fn_0(&ctx);
    printf("x0 = %llu\n", (unsigned long long)ctx.x[0]);
    return 0;
}
)";

    // Write generated C source.
    {
        std::ofstream f(out_c, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.is_open()) << "failed to open " << out_c;
        f << full;
    }

    // Compile with g++ from PATH (link the full runtime + platform for symbol
    // availability).
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

    // Run the compiled binary and capture stdout. On POSIX the current
    // directory is typically not in PATH, so prefix with "./".
#ifdef _WIN32
    std::string run_cmd = "\"" + out_exe + "\"";
#else
    std::string run_cmd = "\"./" + out_exe + "\"";
#endif
    std::string output = RunCapture(run_cmd);
    ASSERT_FALSE(output.empty()) << "compiled binary produced no output";

    EXPECT_NE(output.find("x0 = 1"), std::string::npos)
        << "expected 'x0 = 1' in output, got:\n" << output;
}
