// Emission helpers: turn the AArch64 numeric substrate into host C++ source.
//
// Why this exists: numeric.h defines the *semantics*; a recompiler still has
// to emit those semantics into the generated translation unit. These helpers
// are the bridge -- they produce the exact call expressions that codegen will
// splice in for shift / width / flag / conversion operations, so the generated
// code never falls back to UB-prone plain C expressions.
//
// The generated expressions call `sr::numeric::*` inline functions, so the
// translation unit that includes them must be compiled as C++ (the existing
// end-to-end pipeline already compiles generated code with g++ -std=c++17).
// All helpers are pure string building: header-only, no allocation beyond the
// returned std::string, no dependency outside `common`.
//
// This is the executable feasibility proof for "numeric semantics can be
// emitted into generated code and stay correct under host optimization":
// tests/integration/test_numeric_codegen.cpp emits SelfTestProgram(), compiles
// it with a real compiler at -O2, runs it, and checks UB-boundary results.
#pragma once

#include <string>

namespace sr::numeric::emit {

// Include line every generated translation unit using the substrate needs.
// `include_root` is the -I path that reaches switchrecomp/**.
static inline std::string Prologue() {
    return "#include \"switchrecomp/common/numeric.h\"\n";
}

// Width / shift expressions. `expr` / `amount` are C++ expression strings that
// evaluate to u64. `is32` selects W (32-bit) semantics.
static inline std::string Zext32(const std::string& expr) {
    return "sr::numeric::Zext32(" + expr + ")";
}
static inline std::string Sext32(const std::string& expr) {
    return "sr::numeric::Sext32(" + expr + ")";
}
static inline std::string Shl(const std::string& a, const std::string& amount,
                              bool is32) {
    return std::string("sr::numeric::Shl(") + a + ", " + amount + ", " +
           (is32 ? "true" : "false") + ")";
}
static inline std::string Shr(const std::string& a, const std::string& amount,
                              bool is32) {
    return std::string("sr::numeric::Shr(") + a + ", " + amount + ", " +
           (is32 ? "true" : "false") + ")";
}
static inline std::string Sar(const std::string& a, const std::string& amount,
                              bool is32) {
    return std::string("sr::numeric::Sar(") + a + ", " + amount + ", " +
           (is32 ? "true" : "false") + ")";
}

// ADDS/SUBS: returns a sr::numeric::FlagsResult{value, nzcv}; callers splice
// `.value` / `.nzcv` as needed.
static inline std::string AddWithFlags(const std::string& a,
                                        const std::string& b, bool is32) {
    return std::string("sr::numeric::AddWithFlags(") + a + ", " + b + ", " +
           (is32 ? "true" : "false") + ")";
}
static inline std::string SubWithFlags(const std::string& a,
                                        const std::string& b, bool is32) {
    return std::string("sr::numeric::SubWithFlags(") + a + ", " + b + ", " +
           (is32 ? "true" : "false") + ")";
}
static inline std::string ConditionHolds(const std::string& nzcv,
                                          const std::string& cond) {
    return "sr::numeric::ConditionHolds(" + nzcv + ", " + cond + ")";
}

// FP <-> integer conversions.
static inline std::string FpToSigned(const std::string& v, bool is32) {
    return std::string("sr::numeric::FpToSigned(") + v + ", " +
           (is32 ? "true" : "false") + ")";
}
static inline std::string FpToUnsigned(const std::string& v, bool is32) {
    return std::string("sr::numeric::FpToUnsigned(") + v + ", " +
           (is32 ? "true" : "false") + ")";
}
static inline std::string IntToFp(const std::string& v, bool signed_source,
                                  bool is32_result) {
    return std::string("sr::numeric::IntToFp(") + v + ", " +
           (signed_source ? "true" : "false") + ", " +
           (is32_result ? "true" : "false") + ")";
}

// A complete, self-contained C++ program built *only* from the emit helpers
// above. It prints key=value lines for inputs that are undefined behaviour
// under naive C (shift >= width, 32-bit carry, NaN / out-of-range conversion,
// W high-bit garbage, signed arithmetic shift). The integration test compiles
// this at -O2 and checks every printed value.
static inline std::string SelfTestProgram() {
    std::string s;
    s += Prologue();
    s += "#include <cstdio>\n";
    s += "int main() {\n";
    // 1) W zero-extension drops high garbage.
    s += "  printf(\"zext=%llu\\n\", (unsigned long long)" +
         Zext32("0xFFFFFFFF00000001ull") + ");\n";
    // 2) Shift amount 64 masks to 0 (plain C `1 << 64` is UB); 65 -> 1.
    s += "  printf(\"shl64=%llu\\n\", (unsigned long long)" +
         Shl("1ull", "64ull", false) + ");\n";
    s += "  printf(\"shl65=%llu\\n\", (unsigned long long)" +
         Shl("1ull", "65ull", false) + ");\n";
    // 3) 32-bit add carry: 0xFFFFFFFF + 1 -> value 0, C=1.
    s += "  { auto r = " + AddWithFlags("0xFFFFFFFFull", "1ull", true) +
         "; printf(\"add=%llu c=%u\\n\", (unsigned long long)r.value, "
         "(unsigned)(((r.nzcv) >> 29) & 1u)); }\n";
    // 4) NaN and out-of-range FCVTZS.
    s += "  printf(\"cvt_nan=%lld\\n\", (long long)" +
         FpToSigned("sr::numeric::BitsF64(0x7FF8000000000000ull)", false) +
         ");\n";
    s += "  printf(\"cvt_sat=%lld\\n\", (long long)" +
         FpToSigned("1e300", false) + ");\n";
    // 5) Arithmetic shift right of a negative W value (no signed >>).
    s += "  printf(\"sar=%llx\\n\", (unsigned long long)" +
         Sar("0xFFFFFFFF80000000ull", "4ull", true) + ");\n";
    // 6) Condition code GT after a signed-overflow add (N==V, Z==0 -> true).
    s += "  { auto r = " + AddWithFlags("0x7FFFFFFFull", "1ull", true) +
         "; printf(\"gt=%d\\n\", " + ConditionHolds("r.nzcv", "12") +
         " ? 1 : 0); }\n";
    s += "  return 0;\n}\n";
    return s;
}

} // namespace sr::numeric::emit
