// End-to-end image tests: load a real ELF/NRO fixture, pull the code bytes
// from guest memory, translate -> C with the guest entry as base address,
// compile with g++, run, and assert "x0 = 1".
//
// Skipped when g++ is not available on PATH.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "switchrecomp/codegen/codegen.h"
#include "switchrecomp/common/span.h"
#include "switchrecomp/runtime/loader.h"
#include "switchrecomp/runtime/memory_manager.h"
#include "switchrecomp/translator/translator.h"

#ifndef SR_INCLUDE_DIR
#error "SR_INCLUDE_DIR must be defined via CMake"
#endif
#ifndef SR_TEST_FIXTURES_DIR
#error "SR_TEST_FIXTURES_DIR must be defined via CMake"
#endif

#ifdef _WIN32
#define SR_POPEN _popen
#define SR_PCLOSE _pclose
#else
#define SR_POPEN popen
#define SR_PCLOSE pclose
#endif

namespace {

using sr::common::u64;
using sr::common::u8;
using sr::runtime::LoadedImage;
using sr::runtime::MemoryManager;

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
    const std::string cmd =
        std::string("g++ --version > ") + null_dev + " 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::vector<u8> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
}

std::string ToHex(u64 v) {
    static const char* digits = "0123456789abcdef";
    if (v == 0) return "0";
    std::string s;
    while (v > 0) {
        s.insert(s.begin(), digits[v & 0xF]);
        v >>= 4;
    }
    return s;
}

// Full pipeline for one loaded image. The image entry is used as the
// translator base address so generated function names / branch targets are
// expressed in real guest coordinates.
void RunLoadedImage(const LoadedImage& img, MemoryManager& mm,
                    const std::string& tag) {
    const std::string root = ProjectRoot();
    const std::string include_dir = root + "/include";
    const std::string runtime_dir = root + "/src/runtime";
    const std::string platform_dir = root + "/src/platform";
    const std::string out_c = "e2e_image_" + tag + ".c";
    const std::string out_exe = "e2e_image_" + tag + ".exe";

    // Pull the code bytes out of guest memory.
    void* code_host = mm.GuestToHost(img.entry_point, 8, false);
    ASSERT_NE(code_host, nullptr);
    u8 code[8];
    std::memcpy(code, code_host, 8);

    sr::translator::TranslateOptions topts;
    topts.base_address = img.entry_point;
    topts.strict_decode = true;
    auto translated = sr::translator::Translate(
        sr::common::span<const u8>(code, sizeof(code)), topts);
    ASSERT_TRUE(translated) << translated.error().message;

    auto c_src = sr::codegen::GenerateC(*translated.value(), {});
    ASSERT_TRUE(c_src) << c_src.error().message;

    const std::string fn = "fn_" + ToHex(img.entry_point);
    std::string full = c_src.value();
    full += R"(
#include <stdio.h>
int main() {
    GuestContext ctx;
    for (int i = 0; i < 31; i++) ctx.x[i] = 0;
    ctx.sp = 0; ctx.pc = 0; ctx.nzcv = 0;
    )" + fn + R"((&ctx);
    printf("x0 = %llu\n", (unsigned long long)ctx.x[0]);
    return 0;
}
)";

    {
        std::ofstream f(out_c, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.is_open());
        f << full;
    }

    // Compile + link all runtime and platform sources.
    std::string cmd = "g++ -std=c++17 -I \"" + include_dir + "\" -I \"" +
                      runtime_dir + "\" \"" + out_c + "\" \"" +
                      runtime_dir + "/runtime.cpp\" \"" +
                      runtime_dir + "/memory_manager.cpp\" \"" +
                      runtime_dir + "/svc_table.cpp\" \"" +
                      runtime_dir + "/svc_thread.cpp\" \"" +
                      runtime_dir + "/hle_service.cpp\" \"" +
                      runtime_dir + "/elf_loader.cpp\" \"" +
                      runtime_dir + "/nro_loader.cpp\" \"" +
                      runtime_dir + "/loader_common.cpp\" \"" +
                      platform_dir + "/memory.cpp\" \"" +
                      platform_dir + "/thread.cpp\" -o \"" + out_exe +
                      "\" 2>&1";
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "g++ compile failed:\n" << full;

#ifdef _WIN32
    std::string run_cmd = "\"" + out_exe + "\"";
#else
    std::string run_cmd = "\"./" + out_exe + "\"";
#endif
    std::string output = RunCapture(run_cmd);
    ASSERT_FALSE(output.empty());
    EXPECT_NE(output.find("x0 = 1"), std::string::npos)
        << "expected 'x0 = 1', got:\n" << output;
}

} // namespace

TEST(E2EImage, ElfAddReturnsOne) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e image test";
    }

    const std::vector<u8> data =
        ReadFile(std::string(SR_TEST_FIXTURES_DIR) + "/images/add.elf");
    ASSERT_FALSE(data.empty());

    MemoryManager mm;
    auto result = sr::runtime::LoadElf(
        mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    ASSERT_TRUE(result) << result.error().message;
    RunLoadedImage(result.value(), mm, "elf");
}

TEST(E2EImage, NroAddReturnsOne) {
    if (!HasGxx()) {
        GTEST_SKIP() << "g++ not available on PATH; skipping e2e image test";
    }

    const std::vector<u8> data =
        ReadFile(std::string(SR_TEST_FIXTURES_DIR) + "/images/add.nro");
    ASSERT_FALSE(data.empty());

    MemoryManager mm;
    auto result = sr::runtime::LoadNro(
        mm, sr::common::span<const u8>(data.data(), data.size()), 0);
    ASSERT_TRUE(result) << result.error().message;
    RunLoadedImage(result.value(), mm, "nro");
}
