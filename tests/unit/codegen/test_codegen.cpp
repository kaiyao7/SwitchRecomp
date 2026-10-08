// RC-PIPE-003: codegen C backend — unit tests.
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "switchrecomp/codegen/codegen.h"

using namespace sr::ir;
using sr::codegen::GenerateC;

namespace {

// Build a single-function, single-block module with one instruction appended.
struct SingleInstrModule {
    IRBuilder builder;
    Module module;
    Function* fn;
    BasicBlock* bb;

    SingleInstrModule(u64 guest_addr = 0x1000) {
        fn = module.AddFunction();
        fn->set_guest_address(guest_addr);
        bb = fn->AddBlock();
    }
};

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST(Codegen, Add) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* imm = m.builder.CreateConstant(1, Type::U32());
    m.bb->Append(m.builder.CreateAdd(r0, imm));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = ctx->x[0] + 1"));
}

TEST(Codegen, Sub) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* imm = m.builder.CreateConstant(5, Type::U32());
    m.bb->Append(m.builder.CreateSub(r0, imm));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = ctx->x[0] - 5"));
}

TEST(Codegen, And) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* imm = m.builder.CreateConstant(0xFF, Type::U32());
    m.bb->Append(m.builder.CreateAnd(r0, imm));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = ctx->x[0] & 255"));
}

TEST(Codegen, Or) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* imm = m.builder.CreateConstant(1, Type::U32());
    m.bb->Append(m.builder.CreateOr(r0, imm));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = ctx->x[0] | 1"));
}

TEST(Codegen, Xor) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* imm = m.builder.CreateConstant(1, Type::U32());
    m.bb->Append(m.builder.CreateXor(r0, imm));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = ctx->x[0] ^ 1"));
}

TEST(Codegen, Not) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    m.bb->Append(m.builder.CreateNot(r0));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = ~ctx->x[0]"));
}

TEST(Codegen, Shl) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* imm = m.builder.CreateConstant(2, Type::U32());
    m.bb->Append(m.builder.CreateShl(r0, imm));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = ctx->x[0] << 2"));
}

TEST(Codegen, Shr) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* imm = m.builder.CreateConstant(3, Type::U32());
    m.bb->Append(m.builder.CreateShr(r0, imm));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = (uint64_t)ctx->x[0] >> 3"));
}

TEST(Codegen, Sar) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* imm = m.builder.CreateConstant(4, Type::U32());
    m.bb->Append(m.builder.CreateSar(r0, imm));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = (int64_t)ctx->x[0] >> 4"));
}

TEST(Codegen, Mul) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* imm = m.builder.CreateConstant(6, Type::U32());
    m.bb->Append(m.builder.CreateMul(r0, imm));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = ctx->x[0] * 6"));
}

TEST(Codegen, Load) {
    SingleInstrModule m;
    Constant* addr = m.builder.CreateConstant(0, Type::U64());
    m.bb->Append(m.builder.CreateLoad(addr, Type::U64()));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "ctx->x[0] = *(uint64_t*)sr_rt_guest_to_host(ctx, ctx->x[0], 8, 0)"));
}

TEST(Codegen, Store) {
    SingleInstrModule m;
    Constant* addr = m.builder.CreateConstant(0, Type::U64());
    Constant* val = m.builder.CreateConstant(1, Type::U64());
    m.bb->Append(m.builder.CreateStore(addr, val));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "*(uint64_t*)sr_rt_guest_to_host(ctx, ctx->x[0], 8, 1) = ctx->x[1]"));
}

TEST(Codegen, Branch) {
    SingleInstrModule m;
    BasicBlock* next = m.fn->AddBlock();
    m.bb->Append(m.builder.CreateBranch(next));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "goto bb_"));
}

TEST(Codegen, BranchResolvedLabel) {
    // RC-FIX-002: Branch with a resolved ordinal operand -> goto glabelN.
    SingleInstrModule m;
    Constant* tgt = m.builder.CreateConstant(5, Type::U64());
    m.bb->Append(m.builder.CreateInstruction(
        Opcode::Branch, Type::Void(), {tgt}));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "goto glabel5;"));
}

TEST(Codegen, CondBranch) {
    SingleInstrModule m;
    BasicBlock* bb1 = m.fn->AddBlock();
    Constant* cond = m.builder.CreateConstant(0, Type::U64());
    m.bb->Append(m.builder.CreateCondBranch(cond, bb1, m.bb));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "if (ctx->x[0]) goto bb_1; else goto bb_0;"));
}

TEST(Codegen, Return) {
    SingleInstrModule m;
    m.bb->Append(m.builder.CreateReturn());
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "return;"));
}

TEST(Codegen, Call) {
    SingleInstrModule m;
    m.bb->Append(m.builder.CreateCall(0x1000));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "sr_rt_call(ctx, 4096)"));
}

TEST(Codegen, IndirectBranch) {
    SingleInstrModule m;
    Constant* reg = m.builder.CreateConstant(5, Type::U64());
    m.bb->Append(m.builder.CreateIndirectBranch(reg));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "sr_rt_call(ctx, ctx->x[5])"));
}

TEST(Codegen, Cast) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    m.bb->Append(m.builder.CreateCast(r0, Type::U32()));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "ctx->x[0] = (uint64_t)ctx->x[0]"));
}

TEST(Codegen, Nop) {
    SingleInstrModule m;
    m.bb->Append(m.builder.CreateInstruction(Opcode::Nop, Type::Void(), {}));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "/* nop */"));
}

TEST(Codegen, Svc) {
    SingleInstrModule m;
    m.bb->Append(m.builder.CreateSvc(0x80));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(), "sr_rt_handle_svc(ctx, 128)"));
}

// RC-FIX-002: FCVTZS emits an explicit saturation clamp (no bare out-of-range
// float->int cast, no UB).
TEST(Codegen, FCvtzsSaturating32) {
    SingleInstrModule m;
    Constant* dreg = m.builder.CreateConstant(0, Type::U64());
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());  // FReg s0
    m.bb->Append(m.builder.CreateInstruction(
        Opcode::Convert, Type::S32(), {dreg, s0}));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    const std::string& code = r.value();
    EXPECT_TRUE(Contains(code, "#include <math.h>"));
    EXPECT_TRUE(Contains(code, "isnan(sr_v_get_s(&ctx->v[0]))"));
    EXPECT_TRUE(Contains(code, ">= 2147483648.0f ? 2147483647"));
    EXPECT_TRUE(Contains(code, "<= -2147483648.0f ? (-2147483647 - 1)"));
    // The normal-path cast must still be present for in-range values.
    EXPECT_TRUE(Contains(code, "(int32_t)sr_v_get_s(&ctx->v[0])"));
    // The old unguarded assignment form must be gone.
    EXPECT_FALSE(Contains(code, "] = (int32_t)sr_v_get_s(&ctx->v[0]);"));
}

TEST(Codegen, FCvtzsSaturating64) {
    SingleInstrModule m;
    Constant* dreg = m.builder.CreateConstant(0, Type::U64());
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    m.bb->Append(m.builder.CreateInstruction(
        Opcode::Convert, Type::S64(), {dreg, s0}));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    const std::string& code = r.value();
    EXPECT_TRUE(Contains(code, ">= 9223372036854775808.0"));
    EXPECT_TRUE(Contains(code, "9223372036854775807LL"));
    EXPECT_TRUE(Contains(code, "<= -9223372036854775808.0"));
    EXPECT_TRUE(Contains(code, "(-9223372036854775807LL - 1)"));
    EXPECT_TRUE(Contains(code, "(int64_t)sr_v_get_s(&ctx->v[0])"));
}

// RC-FIX-002: integer CMP writes the same NZCV word; signed/unsigned
// predicates and the overflow expression are all present.
TEST(Codegen, IntegerCompareNZCV64) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* r1 = m.builder.CreateConstant(1, Type::U64());
    m.bb->Append(m.builder.CreateInstruction(
        Opcode::Compare, Type::U64(), {r0, r1}));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    const std::string& code = r.value();
    EXPECT_TRUE(Contains(code, "(int64_t)(ctx->x[0]) < (int64_t)(ctx->x[1])"));
    EXPECT_TRUE(Contains(code, "(uint64_t)(ctx->x[0]) == (uint64_t)(ctx->x[1])"));
    EXPECT_TRUE(Contains(code, "(uint64_t)(ctx->x[0]) >= (uint64_t)(ctx->x[1])"));
    EXPECT_TRUE(Contains(code, ">> 63) << 28"));
}

TEST(Codegen, IntegerCompareNZCV32) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* imm = m.builder.CreateConstant(5, Type::U32());
    m.bb->Append(m.builder.CreateInstruction(
        Opcode::Compare, Type::U32(), {r0, imm}));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    const std::string& code = r.value();
    EXPECT_TRUE(Contains(code, "(int32_t)(ctx->x[0]) < (int32_t)(5)"));
    EXPECT_TRUE(Contains(code, "(uint32_t)(ctx->x[0]) == (uint32_t)(5)"));
    EXPECT_TRUE(Contains(code, "(uint32_t)(ctx->x[0]) >= (uint32_t)(5)"));
    EXPECT_TRUE(Contains(code, ">> 31) << 28"));
}

// RC-FIX-002: integer CMP followed by B.cond LT must render the NZCV test
// against the glabel target of the resolved ordinal.
TEST(Codegen, IntegerCmpThenBranchLT) {
    SingleInstrModule m;
    Constant* r0 = m.builder.CreateConstant(0, Type::U64());
    Constant* r1 = m.builder.CreateConstant(1, Type::U64());
    m.bb->Append(m.builder.CreateInstruction(
        Opcode::Compare, Type::U64(), {r0, r1}));
    m.bb->Append(m.builder.CreateInstruction(
        Opcode::CondBranch, Type::Void(),
        {m.builder.CreateConstant(11, Type::U32()),  // LT
         m.builder.CreateConstant(3, Type::U64())}));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    const std::string& code = r.value();
    EXPECT_TRUE(Contains(code, "goto glabel3"));
    EXPECT_TRUE(Contains(code,
        "((ctx->nzcv >> 31) & 1u) != ((ctx->nzcv >> 28) & 1u)"));
}

// RC-FP-004: FNEG S/D.
TEST(Codegen, FNegS) {
    SingleInstrModule m;
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    Constant* s1 = m.builder.CreateConstant(1, Type::F32());
    m.bb->Append(m.builder.CreateNeg(s0, s1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_s(&ctx->v[0], -sr_v_get_s(&ctx->v[1]))"));
}

TEST(Codegen, FNegD) {
    SingleInstrModule m;
    Constant* d0 = m.builder.CreateConstant(0, Type::F64());
    Constant* d1 = m.builder.CreateConstant(1, Type::F64());
    m.bb->Append(m.builder.CreateNeg(d0, d1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_d(&ctx->v[0], -sr_v_get_d(&ctx->v[1]))"));
}

// RC-FP-004: FABS S/D.
TEST(Codegen, FAbsS) {
    SingleInstrModule m;
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    Constant* s1 = m.builder.CreateConstant(1, Type::F32());
    m.bb->Append(m.builder.CreateAbs(s0, s1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_s(&ctx->v[0], fabsf(sr_v_get_s(&ctx->v[1])))"));
}

TEST(Codegen, FAbsD) {
    SingleInstrModule m;
    Constant* d0 = m.builder.CreateConstant(0, Type::F64());
    Constant* d1 = m.builder.CreateConstant(1, Type::F64());
    m.bb->Append(m.builder.CreateAbs(d0, d1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_d(&ctx->v[0], fabs(sr_v_get_d(&ctx->v[1])))"));
}

// RC-FP-004: FCSEL S.
TEST(Codegen, FCselS) {
    SingleInstrModule m;
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    Constant* s1 = m.builder.CreateConstant(1, Type::F32());
    Constant* cond = m.builder.CreateConstant(11, Type::U32());  // LT
    m.bb->Append(m.builder.CreateSelect(s0, cond, s0, s1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    const std::string& code = r.value();
    // LT predicate over NZCV, with the ternary select form.
    EXPECT_TRUE(Contains(code,
        "((ctx->nzcv >> 31) & 1u) != ((ctx->nzcv >> 28) & 1u)"));
    EXPECT_TRUE(Contains(code,
        "sr_v_set_s(&ctx->v[0], ("));
    EXPECT_TRUE(Contains(code,
        ") ? (sr_v_get_s(&ctx->v[0])) : (sr_v_get_s(&ctx->v[1])));"));
}

// RC-FP-004: FCVT S<->D.
TEST(Codegen, FCvtDoubleToSingle) {
    SingleInstrModule m;
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    Constant* d1 = m.builder.CreateConstant(1, Type::F64());
    m.bb->Append(m.builder.CreateInstruction(
        Opcode::Convert, Type::F32(), {s0, d1}));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_s(&ctx->v[0], (float)sr_v_get_d(&ctx->v[1]))"));
}

TEST(Codegen, FCvtSingleToDouble) {
    SingleInstrModule m;
    Constant* d0 = m.builder.CreateConstant(0, Type::F64());
    Constant* s1 = m.builder.CreateConstant(1, Type::F32());
    m.bb->Append(m.builder.CreateInstruction(
        Opcode::Convert, Type::F64(), {d0, s1}));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_d(&ctx->v[0], (double)sr_v_get_s(&ctx->v[1]))"));
}

// ---- RC-FP-005: FSQRT / FRINT family lowering ----

TEST(Codegen, FSqrtS) {
    SingleInstrModule m;
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    Constant* s1 = m.builder.CreateConstant(1, Type::F32());
    m.bb->Append(m.builder.CreateSqrt(s0, s1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_s(&ctx->v[0], sqrtf(sr_v_get_s(&ctx->v[1])))"));
}

TEST(Codegen, FSqrtD) {
    SingleInstrModule m;
    Constant* d0 = m.builder.CreateConstant(0, Type::F64());
    Constant* d1 = m.builder.CreateConstant(1, Type::F64());
    m.bb->Append(m.builder.CreateSqrt(d0, d1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_d(&ctx->v[0], sqrt(sr_v_get_d(&ctx->v[1])))"));
}

TEST(Codegen, FrintZS) {
    SingleInstrModule m;
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    Constant* s1 = m.builder.CreateConstant(1, Type::F32());
    m.bb->Append(m.builder.CreateFrintZ(s0, s1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_s(&ctx->v[0], truncf(sr_v_get_s(&ctx->v[1])))"));
}

TEST(Codegen, FrintZD) {
    SingleInstrModule m;
    Constant* d0 = m.builder.CreateConstant(0, Type::F64());
    Constant* d1 = m.builder.CreateConstant(1, Type::F64());
    m.bb->Append(m.builder.CreateFrintZ(d0, d1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_d(&ctx->v[0], trunc(sr_v_get_d(&ctx->v[1])))"));
}

TEST(Codegen, FrintPS) {
    SingleInstrModule m;
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    Constant* s1 = m.builder.CreateConstant(1, Type::F32());
    m.bb->Append(m.builder.CreateFrintP(s0, s1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_s(&ctx->v[0], ceilf(sr_v_get_s(&ctx->v[1])))"));
}

TEST(Codegen, FrintMS) {
    SingleInstrModule m;
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    Constant* s1 = m.builder.CreateConstant(1, Type::F32());
    m.bb->Append(m.builder.CreateFrintM(s0, s1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_s(&ctx->v[0], floorf(sr_v_get_s(&ctx->v[1])))"));
}

TEST(Codegen, FrintNS) {
    SingleInstrModule m;
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    Constant* s1 = m.builder.CreateConstant(1, Type::F32());
    m.bb->Append(m.builder.CreateFrintN(s0, s1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_s(&ctx->v[0], rintf(sr_v_get_s(&ctx->v[1])))"));
}

TEST(Codegen, FrintAS) {
    SingleInstrModule m;
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    Constant* s1 = m.builder.CreateConstant(1, Type::F32());
    m.bb->Append(m.builder.CreateFrintA(s0, s1));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_s(&ctx->v[0], roundf(sr_v_get_s(&ctx->v[1])))"));
}

// ---- SIMD stage 2: V128 Mov (MOVI), Broadcast (FDUP), InsertLane (INS),
//      ExtractLane (UMOV), and the lane-wise FADD.4S loop ----

TEST(Codegen, MovV128Movi) {
    SingleInstrModule m;
    Constant* v0 = m.builder.CreateConstant(
        0, Type::V128(LaneKind::F32x4));
    const u64 dup = 0x3fc00000ull | (0x3fc00000ull << 32);
    Constant* payload = m.builder.CreateConstant(
        dup, dup, Type::V128(LaneKind::F32x4));
    m.bb->Append(m.builder.CreateMov(v0, payload));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    const std::string& code = r.value();
    // Local 4-float array initialized with exact literals, then bulk-loaded.
    EXPECT_TRUE(Contains(code, "{ float sr_t"));
    EXPECT_TRUE(Contains(code,
        "[4] = {(float)1.5, (float)1.5, (float)1.5, (float)1.5}"));
    EXPECT_TRUE(Contains(code, "sr_v_load16(&ctx->v[0], sr_t"));
}

TEST(Codegen, BroadcastV128) {
    SingleInstrModule m;
    Constant* v1 = m.builder.CreateConstant(
        1, Type::V128(LaneKind::F32x4));
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    m.bb->Append(m.builder.CreateBroadcast(v1, s0));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "for (int sr_i = 0; sr_i < 4; ++sr_i) "
        "sr_v_set_lane_s(&ctx->v[1], sr_i, sr_v_get_s(&ctx->v[0]));"));
}

TEST(Codegen, InsertLaneV128) {
    SingleInstrModule m;
    Constant* v1 = m.builder.CreateConstant(
        1, Type::V128(LaneKind::F32x4));
    Constant* lane = m.builder.CreateConstant(2, Type::U32());
    Constant* s0 = m.builder.CreateConstant(0, Type::F32());
    m.bb->Append(m.builder.CreateInsertLane(v1, lane, s0));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    EXPECT_TRUE(Contains(r.value(),
        "sr_v_set_lane_s(&ctx->v[1], 2, sr_v_get_s(&ctx->v[0]));"));
}

TEST(Codegen, ExtractLaneV128) {
    SingleInstrModule m;
    Constant* r3 = m.builder.CreateConstant(3, Type::U64());
    Constant* v1 = m.builder.CreateConstant(
        1, Type::V128(LaneKind::F32x4));
    Constant* lane = m.builder.CreateConstant(2, Type::U32());
    m.bb->Append(m.builder.CreateExtractLane(r3, v1, lane));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    const std::string& code = r.value();
    EXPECT_TRUE(Contains(code, "union { float f; uint32_t u; } sr_u"));
    EXPECT_TRUE(Contains(code,
        ".f = sr_v_get_lane_s(&ctx->v[1], 2)"));
    EXPECT_TRUE(Contains(code, "ctx->x[3] = sr_u"));
}

TEST(Codegen, Fadd4SLaneLoop) {
    SingleInstrModule m;
    Constant* v0 = m.builder.CreateConstant(
        0, Type::V128(LaneKind::F32x4));
    Constant* v1 = m.builder.CreateConstant(
        1, Type::V128(LaneKind::F32x4));
    Constant* v2 = m.builder.CreateConstant(
        2, Type::V128(LaneKind::F32x4));
    m.bb->Append(m.builder.CreateInstruction(
        Opcode::Add, Type::V128(LaneKind::F32x4), {v2, v0, v1}));
    auto r = GenerateC(m.module);
    ASSERT_TRUE(r);
    const std::string& code = r.value();
    EXPECT_TRUE(Contains(code, "#pragma omp simd"));
    EXPECT_TRUE(Contains(code,
        "for (int sr_i = 0; sr_i < 4; ++sr_i) "
        "sr_v_set_lane_s(&ctx->v[2], sr_i, "
        "sr_v_get_lane_s(&ctx->v[0], sr_i) + "
        "sr_v_get_lane_s(&ctx->v[1], sr_i))"));
}

// Verify the generated C source actually compiles with gcc -fsyntax-only.
TEST(Codegen, GeneratedCSyntaxValid) {
    IRBuilder b;
    Module module;
    Function* fn = module.AddFunction();
    fn->set_guest_address(0x400000);

    BasicBlock* bb0 = fn->AddBlock();
    BasicBlock* bb1 = fn->AddBlock();

    Constant* r0 = b.CreateConstant(0, Type::U64());
    Constant* r1 = b.CreateConstant(1, Type::U64());
    Constant* imm = b.CreateConstant(1, Type::U32());

    bb0->Append(b.CreateAdd(r0, imm));
    bb0->Append(b.CreateSub(r0, imm));
    bb0->Append(b.CreateAnd(r0, imm));
    bb0->Append(b.CreateOr(r0, imm));
    bb0->Append(b.CreateXor(r0, imm));
    bb0->Append(b.CreateNot(r0));
    bb0->Append(b.CreateShl(r0, imm));
    bb0->Append(b.CreateShr(r0, imm));
    bb0->Append(b.CreateSar(r0, imm));
    bb0->Append(b.CreateMul(r0, imm));
    bb0->Append(b.CreateLoad(r0, Type::U64()));
    bb0->Append(b.CreateStore(r0, r1));
    bb0->Append(b.CreateCondBranch(r0, bb1, bb0));

    // RC-FP-002: FCMP, both conversion directions, and a B.cond-form branch.
    Constant* f0 = b.CreateConstant(0, Type::F32());  // FReg s0 index
    Constant* f1 = b.CreateConstant(1, Type::F32());  // FReg s1 index
    bb0->Append(b.CreateInstruction(Opcode::Compare, Type::Void(), {f0, f1}));
    // RC-FIX-002: integer compare at both widths.
    bb0->Append(b.CreateInstruction(Opcode::Compare, Type::U64(), {r0, r1}));
    bb0->Append(b.CreateInstruction(Opcode::Compare, Type::U32(), {r0, imm}));
    bb0->Append(b.CreateInstruction(Opcode::Convert, Type::S32(),
                                    {b.CreateConstant(0, Type::U64()), f0}));
    bb0->Append(b.CreateInstruction(Opcode::Convert, Type::F32(),
                                    {f0, b.CreateConstant(0, Type::S32())}));
    bb0->Append(b.CreateInstruction(Opcode::CondBranch, Type::Void(),
        {b.CreateConstant(11, Type::U32()), b.CreateConstant(0, Type::U64())}));

    // RC-FP-004: FABS/FNEG/FCSEL S and D, FCVT both directions.
    Constant* d0 = b.CreateConstant(0, Type::F64());
    Constant* d1 = b.CreateConstant(1, Type::F64());
    bb0->Append(b.CreateAbs(f0, f1));
    bb0->Append(b.CreateNeg(d0, d1));
    bb0->Append(b.CreateSelect(f0, b.CreateConstant(11, Type::U32()),
                               f1, f0));
    bb0->Append(b.CreateInstruction(Opcode::Convert, Type::F32(),
                                    {f0, d1}));
    bb0->Append(b.CreateInstruction(Opcode::Convert, Type::F64(),
                                    {d0, f1}));
    // RC-FP-005: FSQRT S/D and all five FRINT variants S.
    bb0->Append(b.CreateSqrt(d0, d1));
    bb0->Append(b.CreateSqrt(f0, f1));
    bb0->Append(b.CreateFrintN(f0, f1));
    bb0->Append(b.CreateFrintP(f0, f1));
    bb0->Append(b.CreateFrintM(f0, f1));
    bb0->Append(b.CreateFrintZ(f0, f1));
    bb0->Append(b.CreateFrintA(f0, f1));

    bb1->Append(b.CreateCall(0x2000));
    bb1->Append(b.CreateIndirectBranch(r1));
    bb1->Append(b.CreateCast(r0, Type::U32()));
    bb1->Append(b.CreateSvc(0x80));
    bb1->Append(b.CreateReturn());

    auto r = GenerateC(module);
    ASSERT_TRUE(r);
    const std::string code = r.value();

    const std::string out_path = "codegen_test_output.c";
    {
        std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.is_open());
        f << code;
    }

    std::string cmd = std::string("C:\\msys64\\ucrt64\\bin\\gcc.exe -fsyntax-only -I ") +
                      SR_INCLUDE_DIR + " " + out_path;
    int rc = std::system(cmd.c_str());
    EXPECT_EQ(rc, 0) << "gcc syntax check failed for generated C:\n" << code;
}
