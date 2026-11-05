// RC-PIPE-002 + RC-PIPE-005: golden tests for the ARM64 instruction set.
// For each canonical sample, translate the 4-byte word and compare
// Module::to_json() output byte-for-byte against the golden fixture.
#include <gtest/gtest.h>

#include "switchrecomp/ir/ir.h"
#include "switchrecomp/translator/translator.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sr::ir;
using namespace sr::translator;

#ifndef SR_TEST_FIXTURES_DIR
#define SR_TEST_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

struct Sample {
    const char* name;
    sr::common::u32 word;
};

// Canonical sample encodings (must match the golden fixtures exactly).
const Sample kSamples[] = {
    {"add_imm",       0x91000400u},
    {"sub_imm",       0xD1000400u},
    {"movz_imm",      0xD28000A0u},
    {"b",             0x14000000u},
    {"nop",           0xD503201Fu},
    {"add_reg",       0x8B010000u},
    {"sub_reg",       0xCB010000u},
    {"and_reg",       0x8A010000u},
    {"orr_reg",       0xAA010000u},
    {"eor_reg",       0xCA010000u},
    {"movn_imm",      0x12800000u},
    {"lsl_imm",       0xD340F800u},
    {"lsr_imm",       0xD341FC00u},
    {"asr_imm",       0x9341FC00u},
    {"ldr_imm",       0xF9400020u},
    {"str_imm",       0xF9000020u},
    {"bl",            0x94000000u},
    {"b_cond",        0x54000000u},
    {"cbz",           0x34000000u},
    {"cbnz",          0x35000000u},
    {"ret",           0xD65F03C0u},
    {"cmp_imm",       0xF100041Fu},
    {"adrp",          0x90000000u},
    {"add_reg_shift", 0x8B010800u},
    {"ldp",           0xE8400440u},
    {"stp",           0xE8000440u},
    {"madd",          0x1B010800u},
    {"msub",          0x1B210800u},
    {"ldr_reg",       0xF8626820u},
    {"str_reg",       0xF8226820u},
    {"movk",          0xF2A000A0u},
    // RC-FP-001: scalar single-precision FP
    {"fadd_s",        0x1E212800u},
    {"fsub_s",        0x1E213800u},
    {"fmul_s",        0x1E210800u},
    {"fmov_imm",      0x1E2F1000u},
    {"fmov_reg",      0x1E204020u},
    {"ldr_s",         0xBD400020u},
    {"str_s",         0xBD000020u},
    // RC-FP-002: FP compare and conversions
    {"fcmp_s",        0x1E212000u},
    {"fcvtzs_s",      0x1E780000u},
    {"scvtf_s",       0x9E220000u},
    // RC-FP-003: scalar double-precision FP
    {"fadd_d",        0x1E612800u},
    {"fmul_d",        0x1E610800u},
    {"fcmp_d",        0x1E612000u},
    {"fcvtzs_d",      0x9E790000u},
    {"scvtf_d",       0x9E230000u},
    {"ldr_d",         0xFD400020u},
    {"str_d",         0xFD000020u},
    // RC-FP-004: FNEG / FABS / FCSEL / FCVT S<->D
    {"fneg_s",        0x1E214000u},
    {"fabs_s",        0x1E20C000u},
    {"fcsel_s",       0x1E21BC00u},
    {"fcvt_sd",       0x1E624000u},  // FCVT Sd,Dn (F64 -> F32)
    {"fcvt_ds",       0x1E22C000u},  // FCVT Dd,Sn (F32 -> F64)
    // RC-FP-005: FSQRT and FRINT family (S forms; D forms in test_decode)
    {"sqrt_s",        0x1E21C000u},
    {"frintn_s",      0x1E244000u},
    {"frintp_s",      0x1E24C000u},
    {"frintm_s",      0x1E254000u},
    {"frintz_s",      0x1E25C000u},
    {"frinta_s",      0x1E264000u},
    // SIMD stage 2: vector move-immediate, broadcast, lane insert/extract,
    // and the F32x4 lane-wise add.
    {"movi_4s",       0x4F03F700u},
    {"fdup_4s",       0x4E040400u},
    {"ins_s",         0x6E140441u},
    {"umov_s",        0x0E143C23u},
    {"fadd_4s",       0x4E21D402u},
};

std::string ReadFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    EXPECT_TRUE(ifs.good()) << "cannot open golden file: " << path;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string s = ss.str();
    // Golden fixtures are stored with LF endings. A checkout performed with
    // core.autocrlf enabled turns them into CRLF, while Module::to_json()
    // always emits LF. Normalise line endings so the comparison is portable.
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        out.push_back(s[i]);
    }
    return out;
}

std::string TranslateToJson(sr::common::u32 word) {
    sr::common::u8 code[4] = {
        static_cast<sr::common::u8>(word),
        static_cast<sr::common::u8>(word >> 8),
        static_cast<sr::common::u8>(word >> 16),
        static_cast<sr::common::u8>(word >> 24),
    };
    TranslateOptions opts;
    opts.strict_decode = true;
    auto result = Translate(
        sr::common::span<const sr::common::u8>(code, 4), opts);
    EXPECT_TRUE(result.ok()) << result.error().message;
    return result.value()->to_json();
}

} // namespace

TEST(TranslatorGolden, AllInstructionsMatchFixture) {
    const std::string dir =
        std::string(SR_TEST_FIXTURES_DIR) + "/translator/golden/";

    for (const auto& s : kSamples) {
        SCOPED_TRACE(std::string("instruction: ") + s.name);
        std::string golden = ReadFile(dir + s.name + ".json");
        std::string actual = TranslateToJson(s.word);
        EXPECT_EQ(actual, golden);
    }
}
