// LDR/STR end-to-end execution tests over the default guest heap.
//
// The runtime maps 1 MB of ReadWrite guest memory at 0x100000 on first use, so
// these programs need no setup: they store through one form and load it back
// through another. Each case exercises the parts the old decoder dropped -
// Rt, the scaled imm12 offset, the access width and sign extension.
//
// Skipped when g++ is not on PATH; compiled with UBSan first, then plainly.
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

TEST(E2EMem, StrbThenLdrb) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // x1 = 0x100000 (default guest heap); store 0x42 as a byte, load it back.
    const uint8_t code[] = {
        0x01, 0x02, 0xA0, 0xD2,  // movz x1,#0x10,lsl#16 0xD2A00201
        0x42, 0x08, 0x80, 0x52,  // movz w2,#0x42        0x52800842
        0x22, 0x00, 0x00, 0x39,  // strb w2,[x1]         0x39000022
        0x20, 0x00, 0x40, 0x39,  // ldrb w0,[x1]         0x39400020
        0xC0, 0x03, 0x5F, 0xD6,  // ret                  0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code), "e2e_mem_byte");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 66)) << "expected 'result = 66', got:\n"
                                         << output;
}

TEST(E2EMem, StrThenLdrAtScaledOffset) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // str x2,[x1,#8] then ldr x0,[x1,#8]: the offset is scaled by the size.
    const uint8_t code[] = {
        0x01, 0x02, 0xA0, 0xD2,  // movz x1,#0x10,lsl#16  0xD2A00201
        0xA2, 0x79, 0x95, 0xD2,  // movz x2,#0xABCD       0xD29579A2
        0x22, 0x04, 0x00, 0xF9,  // str x2,[x1,#8]        0xF9000422
        0x20, 0x04, 0x40, 0xF9,  // ldr x0,[x1,#8]        0xF9400420
        0xC0, 0x03, 0x5F, 0xD6,  // ret                   0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code), "e2e_mem_scaled");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, 43981)) << "expected 'result = 43981', got:\n"
                                             << output;
}

TEST(E2EMem, LdrsbSignExtends) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e compile test";
    }

    // 0x80 stored as a byte, read back with LDRSB X0 -> -128.
    const uint8_t code[] = {
        0x01, 0x02, 0xA0, 0xD2,  // movz x1,#0x10,lsl#16 0xD2A00201
        0x02, 0x10, 0x80, 0x52,  // movz w2,#0x80        0x52801002
        0x22, 0x00, 0x00, 0x39,  // strb w2,[x1]         0x39000022
        0x20, 0x00, 0x80, 0x39,  // ldrsb x0,[x1]        0x39800020
        0xC0, 0x03, 0x5F, 0xD6,  // ret                  0xD65F03C0
    };

    const std::string output = BuildRun(code, sizeof(code), "e2e_mem_ldrsb");
    ASSERT_FALSE(output.empty()) << "build/run pipeline produced no output";
    EXPECT_TRUE(ExpectResult(output, -128)) << "expected 'result = -128', got:\n"
                                           << output;
}
