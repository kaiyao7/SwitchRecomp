// RC-PIPE-004: end-to-end recompiler CLI.
// Translates ARM64 bytecode -> IR -> C, compiles with gcc, runs, verifies x0==1.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "switchrecomp/codegen/codegen.h"
#include "switchrecomp/common/span.h"
#include "switchrecomp/translator/translator.h"

// Cross-platform pipe helpers: _popen/_pclose on Windows, popen/pclose elsewhere.
#ifdef _WIN32
#define SR_POPEN _popen
#define SR_PCLOSE _pclose
#else
#define SR_POPEN popen
#define SR_PCLOSE pclose
#endif

namespace {

// Resolve project root by looking for the `include` directory relative to the
// current working directory. Tries cwd, then cwd/..
std::string FindProjectRoot() {
    auto try_dir = [](const std::string& d) -> bool {
        std::ifstream f(d + "/include/switchrecomp/runtime/runtime.h");
        return f.good();
    };
    if (try_dir(".")) return ".";
    if (try_dir("..")) return "..";
    return ".";
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    const std::string root = FindProjectRoot();
    const std::string include_dir = root + "/include";
    const std::string runtime_cpp = root + "/src/runtime/runtime.cpp";
    const std::string out_c = root + "/build/recomp_output.c";
    const std::string out_exe = root + "/build/recomp_output.exe";

    // Test bytecode: add x0,x0,#1 (0x91000400) ; ret (0xD65F03C0)
    const uint8_t code[] = {0x00, 0x04, 0x00, 0x91, 0xC0, 0x03, 0x5F, 0xD6};

    auto result = sr::translator::Translate(
        sr::common::span<const uint8_t>(code, sizeof(code)), {});
    if (!result) {
        std::fprintf(stderr, "translate failed: %s\n",
                     result.error().message.c_str());
        return 1;
    }

    auto c_src = sr::codegen::GenerateC(*result.value(), {});
    if (!c_src) {
        std::fprintf(stderr, "codegen failed: %s\n",
                     c_src.error().message.c_str());
        return 1;
    }

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

    {
        std::ofstream f(out_c, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            std::fprintf(stderr, "failed to open %s for writing\n",
                         out_c.c_str());
            return 1;
        }
        f << full;
    }

    // Compile generated C with the runtime stub.
    const std::string gcc = "C:\\msys64\\ucrt64\\bin\\gcc.exe";
    std::string cmd = gcc + " -I \"" + include_dir + "\" \"" + out_c + "\" \"" +
                      runtime_cpp + "\" -o \"" + out_exe + "\" 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "gcc compile failed (rc=%d)\n", rc);
        return 1;
    }

    // Run and capture stdout. Use backslashes so cmd.exe (via popen)
    // parses the path correctly.
    std::string run_exe = out_exe;
    for (char& c : run_exe) if (c == '/') c = '\\';
    std::string run_cmd = "\"" + run_exe + "\"";
    FILE* pipe = SR_POPEN(run_cmd.c_str(), "r");
    if (!pipe) {
        std::fprintf(stderr, "failed to run %s\n", out_exe.c_str());
        return 1;
    }
    std::string output;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    SR_PCLOSE(pipe);

    std::printf("%s", output.c_str());

    if (output.find("x0 = 1") == std::string::npos) {
        std::fprintf(stderr, "verification failed: expected 'x0 = 1' in output\n");
        return 1;
    }
    return 0;
}
