// RC-PIPE-007: end-to-end cross-function call test.
// Translates two ARM64 functions (A at 0x100, B at 0x200) to C, compiles and
// runs, and asserts that A -> BL B -> mov x0,#42 -> ret returns x0 == 42.
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

// Cross-platform pipe helpers.
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
    int rc = std::system(cmd.c_str());
    return rc == 0;
}

// Translate a single guest function at |base_address| and return the generated
// C source.
std::string TranslateFn(const uint8_t* code, size_t size, uint64_t base_address) {
    sr::translator::TranslateOptions opts;
    opts.base_address = base_address;
    opts.strict_decode = true;
    auto result = sr::translator::Translate(
        sr::common::span<const uint8_t>(code, size), opts);
    if (!result) {
        ADD_FAILURE() << "translate failed: " << result.error().message;
        return "";
    }
    auto c_src = sr::codegen::GenerateC(*result.value(), {});
    if (!c_src) {
        ADD_FAILURE() << "codegen failed: " << c_src.error().message;
        return "";
    }
    return c_src.value();
}

// Strip the codegen preamble (includes, GuestContext typedef, sr_rt_*
// declarations) from a generated source, returning only the function bodies.
// The preamble ends right before the first "void fn_" line.
std::string StripPreamble(const std::string& src) {
    const std::string marker = "void fn_";
    auto pos = src.find(marker);
    if (pos == std::string::npos) return src;
    return src.substr(pos);
}

}  // namespace

TEST(E2E, CallReturns42) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    const std::string root = ProjectRoot();
    const std::string include_dir = root + "/include";
    const std::string runtime_dir = root + "/src/runtime";
    const std::string platform_dir = root + "/src/platform";
    const std::string out_c = "e2e_call_output.c";
    const std::string out_exe = "e2e_call_output.exe";

    // Function A at 0x100: bl 0x200 ; ret
    //   BL offset = 0x200 - 0x100 = 0x100, imm26 = 0x40
    //   encoding = 0x94000000 | 0x40 = 0x94000040 (LE: 40 00 00 94)
    const uint8_t code_a[] = {
        0x40, 0x00, 0x00, 0x94,  // bl #0x100  (-> 0x200)
        0xC0, 0x03, 0x5F, 0xD6   // ret
    };

    // Function B at 0x200: movz x0, #42 ; ret
    //   movz x0, #42: 0xD2800000 | (42 << 5) = 0xD2800540 (LE: 40 05 80 D2)
    const uint8_t code_b[] = {
        0x40, 0x05, 0x80, 0xD2,  // mov x0, #42
        0xC0, 0x03, 0x5F, 0xD6   // ret
    };

    // Translate both functions. The first translation carries the full
    // preamble; subsequent ones are stripped to avoid duplicate declarations.
    std::string full = TranslateFn(code_a, sizeof(code_a), 0x100);
    ASSERT_FALSE(full.empty());

    std::string src_b = TranslateFn(code_b, sizeof(code_b), 0x200);
    ASSERT_FALSE(src_b.empty());
    full += StripPreamble(src_b);

    // Append a registration function and host main().
    full += R"(
void register_functions() {
    sr_rt_register_function(0x100, fn_100);
    sr_rt_register_function(0x200, fn_200);
}

#include <stdio.h>
int main() {
    GuestContext ctx;
    for (int i = 0; i < 31; i++) ctx.x[i] = 0;
    ctx.sp = 0; ctx.pc = 0; ctx.nzcv = 0;
    register_functions();
    fn_100(&ctx);
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

    // Compile with g++, linking the full runtime + platform for symbol
    // availability (sr_rt_call, sr_rt_register_function, etc.).
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

    EXPECT_NE(output.find("x0 = 42"), std::string::npos)
        << "expected 'x0 = 42' in output, got:\n" << output;
}
