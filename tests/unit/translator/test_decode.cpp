// RC-PIPE-002 + RC-PIPE-005: ARM64 -> IR decode — unit tests.
#include <gtest/gtest.h>

#include "switchrecomp/ir/ir.h"
#include "switchrecomp/translator/translator.h"

using namespace sr::ir;
using namespace sr::translator;

namespace {

// Helper: translate a single 4-byte instruction and return the module.
std::unique_ptr<Module> TranslateOne(const sr::common::u8* bytes) {
    TranslateOptions opts;
    opts.strict_decode = true;
    auto result = Translate(sr::common::span<const sr::common::u8>(bytes, 4),
                            opts);
    EXPECT_TRUE(result.ok()) << result.error().message;
    return std::move(result.value());
}

// Decode a little-endian 32-bit word into a 4-byte array.
void WordToBytes(sr::common::u32 word, sr::common::u8 out[4]) {
    out[0] = static_cast<sr::common::u8>(word);
    out[1] = static_cast<sr::common::u8>(word >> 8);
    out[2] = static_cast<sr::common::u8>(word >> 16);
    out[3] = static_cast<sr::common::u8>(word >> 24);
}

void ExpectOneInstr(const sr::common::u8* bytes, Opcode expected_op,
                    size_t expected_operands) {
    auto module = TranslateOne(bytes);
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(module->verify());

    const auto& fns = module->functions();
    ASSERT_EQ(fns.size(), 1u);
    const auto& blocks = fns[0]->blocks();
    ASSERT_EQ(blocks.size(), 1u);
    const auto& instrs = blocks[0]->instructions();
    ASSERT_EQ(instrs.size(), 1u);
    EXPECT_EQ(instrs[0]->opcode(), expected_op);
    EXPECT_EQ(instrs[0]->operands().size(), expected_operands);
}

// For compound opcodes that emit multiple IR instructions: verify the
// instruction count and the opcode + operand count of the first instruction.
void ExpectFirstInstr(const sr::common::u8* bytes, size_t expected_count,
                      Opcode expected_first_op,
                      size_t expected_first_operands) {
    auto module = TranslateOne(bytes);
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(module->verify());

    const auto& fns = module->functions();
    ASSERT_EQ(fns.size(), 1u);
    const auto& blocks = fns[0]->blocks();
    ASSERT_EQ(blocks.size(), 1u);
    const auto& instrs = blocks[0]->instructions();
    ASSERT_EQ(instrs.size(), expected_count);
    EXPECT_EQ(instrs[0]->opcode(), expected_first_op);
    EXPECT_EQ(instrs[0]->operands().size(), expected_first_operands);
}

} // namespace

// ---- Existing 5 (kept for regression) ----

TEST(Translate, AddImmediate) {
    sr::common::u8 code[4];
    WordToBytes(0x91000400u, code); // ADD x0, x0, #1
    ExpectOneInstr(code, Opcode::Add, 2u);
}

TEST(Translate, SubImmediate) {
    sr::common::u8 code[4];
    WordToBytes(0xD1000400u, code); // SUB x0, x0, #1
    ExpectOneInstr(code, Opcode::Sub, 2u);
}

TEST(Translate, MovImmediate) {
    sr::common::u8 code[4];
    WordToBytes(0xD28000A0u, code); // MOVZ x0, #5 -> Mov(rd, imm)
    ExpectOneInstr(code, Opcode::Mov, 2u);
}

// RC-FIX-002: MOVZ must target its actual Rd.
TEST(Translate, MovzOtherRegister) {
    sr::common::u8 code[4];
    WordToBytes(0xD2800041u, code); // MOVZ x1, #2
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    const auto& instrs =
        module->functions()[0]->blocks()[0]->instructions();
    ASSERT_EQ(instrs.size(), 1u);
    EXPECT_EQ(instrs[0]->opcode(), Opcode::Mov);
    auto* dst = dynamic_cast<Constant*>(instrs[0]->operands()[0]);
    ASSERT_NE(dst, nullptr);
    EXPECT_EQ(dst->value(), 1u);
}

TEST(Translate, Branch) {
    sr::common::u8 code[4];
    WordToBytes(0x14000000u, code); // B #0 (resolved target ordinal operand)
    ExpectOneInstr(code, Opcode::Branch, 1u);
}

TEST(Translate, Nop) {
    sr::common::u8 code[4];
    WordToBytes(0xD503201Fu, code); // NOP
    ExpectOneInstr(code, Opcode::Nop, 0u);
}

// ---- New instructions (RC-PIPE-002) ----

TEST(Translate, AddRegister) {
    sr::common::u8 code[4];
    WordToBytes(0x8B010000u, code); // ADD x0, x0, x1
    ExpectOneInstr(code, Opcode::Add, 2u);
}

TEST(Translate, SubRegister) {
    sr::common::u8 code[4];
    WordToBytes(0xCB010000u, code); // SUB x0, x0, x1
    ExpectOneInstr(code, Opcode::Sub, 2u);
}

TEST(Translate, And) {
    sr::common::u8 code[4];
    WordToBytes(0x8A010000u, code); // AND x0, x0, x1
    ExpectOneInstr(code, Opcode::And, 2u);
}

TEST(Translate, Orr) {
    sr::common::u8 code[4];
    WordToBytes(0xAA010000u, code); // ORR x0, x0, x1
    ExpectOneInstr(code, Opcode::Or, 2u);
}

TEST(Translate, Eor) {
    sr::common::u8 code[4];
    WordToBytes(0xCA010000u, code); // EOR x0, x0, x1
    ExpectOneInstr(code, Opcode::Xor, 2u);
}

TEST(Translate, Movn) {
    sr::common::u8 code[4];
    WordToBytes(0x12800000u, code); // MOVN x0, #0
    ExpectOneInstr(code, Opcode::Not, 1u);
}

TEST(Translate, LslImmediate) {
    sr::common::u8 code[4];
    WordToBytes(0xD340F800u, code); // LSL x0, x0, #1 (UBFM immr=0, imms=62)
    ExpectOneInstr(code, Opcode::Shl, 2u);
}

TEST(Translate, LsrImmediate) {
    sr::common::u8 code[4];
    WordToBytes(0xD341FC00u, code); // LSR x0, x0, #1 (UBFM immr=1, imms=63)
    ExpectOneInstr(code, Opcode::Shr, 2u);
}

TEST(Translate, AsrImmediate) {
    sr::common::u8 code[4];
    WordToBytes(0x9341FC00u, code); // ASR x0, x0, #1 (SBFM immr=1, imms=63)
    ExpectOneInstr(code, Opcode::Sar, 2u);
}

TEST(Translate, LdrUnsignedOffset) {
    sr::common::u8 code[4];
    WordToBytes(0xF9400020u, code); // LDR x0, [x1]
    ExpectOneInstr(code, Opcode::Load, 1u);
}

TEST(Translate, StrUnsignedOffset) {
    sr::common::u8 code[4];
    WordToBytes(0xF9000020u, code); // STR x0, [x1]
    ExpectOneInstr(code, Opcode::Store, 2u);
}

TEST(Translate, BranchWithLink) {
    sr::common::u8 code[4];
    WordToBytes(0x94000000u, code); // BL #0
    ExpectOneInstr(code, Opcode::Call, 1u);
}

TEST(Translate, BranchConditional) {
    sr::common::u8 code[4];
    WordToBytes(0x54000000u, code); // B.EQ #0
    // RC-FP-002: emits {U32 cond, U64 target address}.
    ExpectOneInstr(code, Opcode::CondBranch, 2u);
}

TEST(Translate, CompareBranchZero) {
    sr::common::u8 code[4];
    WordToBytes(0x34000000u, code); // CBZ x0, #0
    ExpectOneInstr(code, Opcode::CondBranch, 1u);
}

TEST(Translate, CompareBranchNonZero) {
    sr::common::u8 code[4];
    WordToBytes(0x35000000u, code); // CBNZ x0, #0
    ExpectOneInstr(code, Opcode::CondBranch, 1u);
}

TEST(Translate, Ret) {
    sr::common::u8 code[4];
    WordToBytes(0xD65F03C0u, code); // RET
    ExpectOneInstr(code, Opcode::Return, 0u);
}

TEST(Translate, CmpImmediate) {
    sr::common::u8 code[4];
    WordToBytes(0xF100041Fu, code); // CMP x0, #1 -> Compare (writes NZCV)
    ExpectOneInstr(code, Opcode::Compare, 2u);
}

// RC-FIX-002: CMP register alias and non-alias SUBS regressions.
TEST(Translate, CmpRegister) {
    sr::common::u8 code[4];
    WordToBytes(0xEB01001Fu, code); // CMP x0, x1 -> Compare
    ExpectOneInstr(code, Opcode::Compare, 2u);
}

TEST(Translate, CmpRegister32) {
    sr::common::u8 code[4];
    WordToBytes(0x6B01001Fu, code); // CMP w0, w1 -> Compare (U32 width)
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    const auto& instrs =
        module->functions()[0]->blocks()[0]->instructions();
    ASSERT_EQ(instrs.size(), 1u);
    EXPECT_EQ(instrs[0]->opcode(), Opcode::Compare);
    EXPECT_EQ(instrs[0]->type().id, TypeId::U32);
}

TEST(Translate, SubsRegisterNotCmp) {
    sr::common::u8 code[4];
    WordToBytes(0xEB010000u, code); // SUBS x0, x0, x1 (Rd=0): stays Sub
    ExpectOneInstr(code, Opcode::Sub, 2u);
}

TEST(Translate, SubsImmediateNotCmp) {
    sr::common::u8 code[4];
    WordToBytes(0xF1000400u, code); // SUBS x0, x0, #1 (Rd=0): stays Sub
    ExpectOneInstr(code, Opcode::Sub, 2u);
}

// ---- Extended instructions (RC-PIPE-005) ----

TEST(Translate, Adrp) {
    sr::common::u8 code[4];
    WordToBytes(0x90000000u, code); // ADRP x0, #0
    ExpectOneInstr(code, Opcode::Add, 2u);
}

TEST(Translate, AddRegisterShift) {
    sr::common::u8 code[4];
    WordToBytes(0x8B010800u, code); // ADD x0, x0, x1, LSL #2
    ExpectFirstInstr(code, 2u, Opcode::Shl, 2u);
}

TEST(Translate, Ldp) {
    sr::common::u8 code[4];
    WordToBytes(0xE8400440u, code); // LDP x0, x1, [x2]
    ExpectFirstInstr(code, 4u, Opcode::Add, 2u);
}

TEST(Translate, Stp) {
    sr::common::u8 code[4];
    WordToBytes(0xE8000440u, code); // STP x0, x1, [x2]
    ExpectFirstInstr(code, 4u, Opcode::Add, 2u);
}

TEST(Translate, Madd) {
    sr::common::u8 code[4];
    WordToBytes(0x1B010800u, code); // MADD x0, x0, x1, x2
    ExpectFirstInstr(code, 2u, Opcode::Mul, 2u);
}

TEST(Translate, Msub) {
    sr::common::u8 code[4];
    WordToBytes(0x1B210800u, code); // MSUB x0, x0, x1, x2
    ExpectFirstInstr(code, 2u, Opcode::Mul, 2u);
}

TEST(Translate, LdrRegisterOffset) {
    sr::common::u8 code[4];
    WordToBytes(0xF8626820u, code); // LDR x0, [x1, x2]
    ExpectFirstInstr(code, 2u, Opcode::Add, 2u);
}

TEST(Translate, StrRegisterOffset) {
    sr::common::u8 code[4];
    WordToBytes(0xF8226820u, code); // STR x0, [x1, x2]
    ExpectFirstInstr(code, 2u, Opcode::Add, 2u);
}

TEST(Translate, Movk) {
    sr::common::u8 code[4];
    WordToBytes(0xF2A000A0u, code); // MOVK x0, #5, LSL #16
    ExpectFirstInstr(code, 2u, Opcode::And, 2u);
}

// ---- RC-FP-001: scalar single-precision FP ----

TEST(Translate, FaddS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E212800u, code); // FADD s0, s0, s1
    ExpectOneInstr(code, Opcode::Add, 3u);
}

TEST(Translate, FsubS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E213800u, code); // FSUB s0, s0, s1
    ExpectOneInstr(code, Opcode::Sub, 3u);
}

TEST(Translate, FmulS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E210800u, code); // FMUL s0, s0, s1
    ExpectOneInstr(code, Opcode::Mul, 3u);
}

TEST(Translate, FmovImmS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E2F1000u, code); // FMOV s0, #1.5
    ExpectOneInstr(code, Opcode::Mov, 2u);
}

TEST(Translate, FmovRegS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E204020u, code); // FMOV s0, s1
    ExpectOneInstr(code, Opcode::Mov, 2u);
}

TEST(Translate, LdrS) {
    sr::common::u8 code[4];
    WordToBytes(0xBD400020u, code); // LDR s0, [x1]
    ExpectOneInstr(code, Opcode::Load, 1u);
}

TEST(Translate, StrS) {
    sr::common::u8 code[4];
    WordToBytes(0xBD000020u, code); // STR s0, [x1]
    ExpectOneInstr(code, Opcode::Store, 2u);
}

// Verify FP arithmetic operands carry F32 types and the right register indices.
TEST(Translate, FaddSOperandTypes) {
    sr::common::u8 code[4];
    WordToBytes(0x1E212800u, code); // FADD s0, s0, s1
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    Instruction* add =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(add->type().id, TypeId::F32);
    ASSERT_EQ(add->operands().size(), 3u);
    for (Value* op : add->operands()) {
        EXPECT_EQ(op->type().id, TypeId::F32);
    }
    auto* rd = add->operands()[0];
    auto* rn = add->operands()[1];
    auto* rm = add->operands()[2];
    EXPECT_EQ(rd->id() != 0, true);
    EXPECT_EQ(dynamic_cast<Constant*>(rd)->value(), 0u);
    EXPECT_EQ(dynamic_cast<Constant*>(rn)->value(), 0u);
    EXPECT_EQ(dynamic_cast<Constant*>(rm)->value(), 1u);
}

// ---- RC-FP-002: FCMP / FCVTZS / SCVTF ----

TEST(Translate, FCmpS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E212000u, code); // FCMP s0, s1
    ExpectOneInstr(code, Opcode::Compare, 2u);
}

TEST(Translate, FCmpSZero) {
    sr::common::u8 code[4];
    WordToBytes(0x1E3F2000u, code); // FCMP s0, #0.0
    ExpectOneInstr(code, Opcode::Compare, 2u);
}

TEST(Translate, FCvtzsS32) {
    sr::common::u8 code[4];
    WordToBytes(0x1E780000u, code); // FCVTZS w0, s0
    ExpectOneInstr(code, Opcode::Convert, 2u);
}

TEST(Translate, FCvtzsS64) {
    sr::common::u8 code[4];
    WordToBytes(0x9E780000u, code); // FCVTZS x0, s0
    ExpectOneInstr(code, Opcode::Convert, 2u);
}

TEST(Translate, SCvtfSFrom32) {
    sr::common::u8 code[4];
    WordToBytes(0x1E220000u, code); // SCVTF s0, w0
    ExpectOneInstr(code, Opcode::Convert, 2u);
}

TEST(Translate, SCvtfSFrom64) {
    sr::common::u8 code[4];
    WordToBytes(0x9E220000u, code); // SCVTF s0, x0
    ExpectOneInstr(code, Opcode::Convert, 2u);
}

// Verify Convert carries the destination type and width-correct register refs.
TEST(Translate, FcvtzsConvertTypes) {
    sr::common::u8 code[4];
    WordToBytes(0x1E780000u, code); // FCVTZS w0, s0
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    Instruction* conv =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(conv->opcode(), Opcode::Convert);
    EXPECT_EQ(conv->type().id, TypeId::S32);
    ASSERT_EQ(conv->operands().size(), 2u);
    // dst = integer register (U64 index), src = FReg (F32 index).
    EXPECT_EQ(conv->operands()[0]->type().id, TypeId::U64);
    EXPECT_EQ(conv->operands()[1]->type().id, TypeId::F32);
    EXPECT_EQ(dynamic_cast<Constant*>(conv->operands()[0])->value(), 0u);
    EXPECT_EQ(dynamic_cast<Constant*>(conv->operands()[1])->value(), 0u);
}

TEST(Translate, ScvtfConvertTypes) {
    sr::common::u8 code[4];
    WordToBytes(0x9E220000u, code); // SCVTF s0, x0
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    Instruction* conv =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(conv->opcode(), Opcode::Convert);
    EXPECT_EQ(conv->type().id, TypeId::F32);
    ASSERT_EQ(conv->operands().size(), 2u);
    // dst = FReg, src = S64-typed register index (source width marker).
    EXPECT_EQ(conv->operands()[0]->type().id, TypeId::F32);
    EXPECT_EQ(conv->operands()[1]->type().id, TypeId::S64);
}

// ---- RC-FP-003: scalar double-precision FP ----

TEST(Translate, FaddD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E612800u, code); // FADD d0, d0, d1
    ExpectOneInstr(code, Opcode::Add, 3u);
}

TEST(Translate, FsubD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E613800u, code); // FSUB d0, d0, d1
    ExpectOneInstr(code, Opcode::Sub, 3u);
}

TEST(Translate, FmulD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E610800u, code); // FMUL d0, d0, d1
    ExpectOneInstr(code, Opcode::Mul, 3u);
}

TEST(Translate, FdivD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E611800u, code); // FDIV d0, d0, d1
    ExpectOneInstr(code, Opcode::Div, 3u);
}

TEST(Translate, FmovRegD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E604020u, code); // FMOV d0, d1
    ExpectOneInstr(code, Opcode::Mov, 2u);
}

TEST(Translate, FCmpD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E612000u, code); // FCMP d0, d1
    ExpectOneInstr(code, Opcode::Compare, 2u);
}

TEST(Translate, FCmpDZero) {
    sr::common::u8 code[4];
    WordToBytes(0x1E7F2000u, code); // FCMP d0, #0.0
    ExpectOneInstr(code, Opcode::Compare, 2u);
}

TEST(Translate, FCvtzsD32) {
    sr::common::u8 code[4];
    WordToBytes(0x1E790000u, code); // FCVTZS w0, d0
    ExpectOneInstr(code, Opcode::Convert, 2u);
}

TEST(Translate, FCvtzsD64) {
    sr::common::u8 code[4];
    WordToBytes(0x9E790000u, code); // FCVTZS x0, d0
    ExpectOneInstr(code, Opcode::Convert, 2u);
}

TEST(Translate, SCvtfDFrom32) {
    sr::common::u8 code[4];
    WordToBytes(0x1E230000u, code); // SCVTF d0, w0
    ExpectOneInstr(code, Opcode::Convert, 2u);
}

TEST(Translate, SCvtfDFrom64) {
    sr::common::u8 code[4];
    WordToBytes(0x9E230000u, code); // SCVTF d0, x0
    ExpectOneInstr(code, Opcode::Convert, 2u);
}

TEST(Translate, LdrD) {
    sr::common::u8 code[4];
    WordToBytes(0xFD400020u, code); // LDR d0, [x1]
    ExpectOneInstr(code, Opcode::Load, 1u);
}

TEST(Translate, StrD) {
    sr::common::u8 code[4];
    WordToBytes(0xFD000020u, code); // STR d0, [x1]
    ExpectOneInstr(code, Opcode::Store, 2u);
}

// Verify F64 arithmetic operands carry F64 types and the right indices.
TEST(Translate, FaddDOperandTypes) {
    sr::common::u8 code[4];
    WordToBytes(0x1E612800u, code); // FADD d0, d0, d1
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    Instruction* add =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(add->type().id, TypeId::F64);
    ASSERT_EQ(add->operands().size(), 3u);
    for (Value* op : add->operands()) {
        EXPECT_EQ(op->type().id, TypeId::F64);
    }
    EXPECT_EQ(dynamic_cast<Constant*>(add->operands()[0])->value(), 0u);
    EXPECT_EQ(dynamic_cast<Constant*>(add->operands()[1])->value(), 0u);
    EXPECT_EQ(dynamic_cast<Constant*>(add->operands()[2])->value(), 1u);
}

// Verify double Convert directions: F64->S64 and S64->F64.
TEST(Translate, FcvtzsDConvertTypes) {
    sr::common::u8 code[4];
    WordToBytes(0x9E790000u, code); // FCVTZS x0, d0
    auto module = TranslateOne(code);
    Instruction* conv =
        module->functions()[0]->blocks()[0]->instructions()[0];
    EXPECT_EQ(conv->type().id, TypeId::S64);
    EXPECT_EQ(conv->operands()[0]->type().id, TypeId::U64);
    EXPECT_EQ(conv->operands()[1]->type().id, TypeId::F64);
}

TEST(Translate, ScvtfDConvertTypes) {
    sr::common::u8 code[4];
    WordToBytes(0x9E230000u, code); // SCVTF d0, x0
    auto module = TranslateOne(code);
    Instruction* conv =
        module->functions()[0]->blocks()[0]->instructions()[0];
    EXPECT_EQ(conv->type().id, TypeId::F64);
    EXPECT_EQ(conv->operands()[0]->type().id, TypeId::F64);
    EXPECT_EQ(conv->operands()[1]->type().id, TypeId::S64);
}

// S words must not decode as D and vice versa (mask/value separation).
TEST(Translate, SDEncodingsStayDistinct) {
    // FADD S with s1 (0x1E212800) must remain F32, not match the D group.
    sr::common::u8 bytes_s[4];
    WordToBytes(0x1E212800u, bytes_s);
    auto ms = TranslateOne(bytes_s);
    Instruction* is =
        ms->functions()[0]->blocks()[0]->instructions()[0];
    EXPECT_EQ(is->opcode(), Opcode::Add);
    EXPECT_EQ(is->type().id, TypeId::F32);

    // FCVTZS S,X (0x9E780000) must keep F32 source.
    sr::common::u8 bytes_c[4];
    WordToBytes(0x9E780000u, bytes_c);
    auto mc = TranslateOne(bytes_c);
    Instruction* ic =
        mc->functions()[0]->blocks()[0]->instructions()[0];
    EXPECT_EQ(ic->operands()[1]->type().id, TypeId::F32);
}

// ---- RC-FP-004: FNEG / FABS / FCSEL / FCVT S<->D ----

TEST(Translate, FNegS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E214000u, code); // FNEG s0, s0
    ExpectOneInstr(code, Opcode::Neg, 2u);
}

TEST(Translate, FNegD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E614000u, code); // FNEG d0, d0
    ExpectOneInstr(code, Opcode::Neg, 2u);
}

TEST(Translate, FAbsS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E20C000u, code); // FABS s0, s0
    ExpectOneInstr(code, Opcode::Abs, 2u);
}

TEST(Translate, FAbsD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E60C000u, code); // FABS d0, d0
    ExpectOneInstr(code, Opcode::Abs, 2u);
}

TEST(Translate, FCselS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E21BC00u, code); // FCSEL s0, s0, s1, LT
    ExpectOneInstr(code, Opcode::Select, 4u);
}

TEST(Translate, FCselD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E61BC00u, code); // FCSEL d0, d0, d1, LT
    ExpectOneInstr(code, Opcode::Select, 4u);
}

TEST(Translate, FcvtSToD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E22C000u, code); // FCVT d0, s0
    ExpectOneInstr(code, Opcode::Convert, 2u);
}

TEST(Translate, FcvtDToS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E624000u, code); // FCVT s0, d0
    ExpectOneInstr(code, Opcode::Convert, 2u);
}

// Verify FNEG register fields and F32 typing with distinct src/dst.
TEST(Translate, FNegSFields) {
    sr::common::u8 code[4];
    WordToBytes(0x1E214062u, code); // FNEG s2, s3
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    Instruction* neg =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(neg->opcode(), Opcode::Neg);
    EXPECT_EQ(neg->type().id, TypeId::F32);
    ASSERT_EQ(neg->operands().size(), 2u);
    EXPECT_EQ(neg->operands()[0]->type().id, TypeId::F32);
    EXPECT_EQ(neg->operands()[1]->type().id, TypeId::F32);
    EXPECT_EQ(dynamic_cast<Constant*>(neg->operands()[0])->value(), 2u);
    EXPECT_EQ(dynamic_cast<Constant*>(neg->operands()[1])->value(), 3u);
}

// Verify FCSEL operands: dst, U32 cond code, Rn (true), Rm (false).
TEST(Translate, FCselSFields) {
    sr::common::u8 code[4];
    WordToBytes(0x1E21BC00u, code); // FCSEL s0, s0, s1, LT
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    Instruction* sel =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(sel->opcode(), Opcode::Select);
    EXPECT_EQ(sel->type().id, TypeId::F32);
    ASSERT_EQ(sel->operands().size(), 4u);
    EXPECT_EQ(sel->operands()[0]->type().id, TypeId::F32);
    auto* cond = dynamic_cast<Constant*>(sel->operands()[1]);
    ASSERT_NE(cond, nullptr);
    EXPECT_EQ(cond->type().id, TypeId::U32);
    EXPECT_EQ(cond->value(), 11u);  // LT
    EXPECT_EQ(dynamic_cast<Constant*>(sel->operands()[2])->value(), 0u);
    EXPECT_EQ(dynamic_cast<Constant*>(sel->operands()[3])->value(), 1u);
}

// Verify FCVT direction typing for both forms.
TEST(Translate, FcvtConvertDirections) {
    // S -> D: instruction type F64; dst is DReg (F64), src is FReg (F32).
    sr::common::u8 bytes_ds[4];
    WordToBytes(0x1E22C000u, bytes_ds);
    auto m_ds = TranslateOne(bytes_ds);
    Instruction* i_ds =
        m_ds->functions()[0]->blocks()[0]->instructions()[0];
    EXPECT_EQ(i_ds->type().id, TypeId::F64);
    EXPECT_EQ(i_ds->operands()[0]->type().id, TypeId::F64);
    EXPECT_EQ(i_ds->operands()[1]->type().id, TypeId::F32);

    // D -> S: instruction type F32; dst FReg, src DReg.
    sr::common::u8 bytes_sd[4];
    WordToBytes(0x1E624000u, bytes_sd);
    auto m_sd = TranslateOne(bytes_sd);
    Instruction* i_sd =
        m_sd->functions()[0]->blocks()[0]->instructions()[0];
    EXPECT_EQ(i_sd->type().id, TypeId::F32);
    EXPECT_EQ(i_sd->operands()[0]->type().id, TypeId::F32);
    EXPECT_EQ(i_sd->operands()[1]->type().id, TypeId::F64);
}

// D FNEG/FABS must not decode as S and vice versa.
TEST(Translate, FPMiscSDStayDistinct) {
    sr::common::u8 bytes_d[4];
    WordToBytes(0x1E614000u, bytes_d);  // FNEG d0, d0
    auto md = TranslateOne(bytes_d);
    Instruction* id =
        md->functions()[0]->blocks()[0]->instructions()[0];
    EXPECT_EQ(id->type().id, TypeId::F64);
    EXPECT_EQ(id->operands()[0]->type().id, TypeId::F64);

    sr::common::u8 bytes_s[4];
    WordToBytes(0x1E20C000u, bytes_s);  // FABS s0, s0
    auto ms = TranslateOne(bytes_s);
    Instruction* is =
        ms->functions()[0]->blocks()[0]->instructions()[0];
    EXPECT_EQ(is->type().id, TypeId::F32);
    EXPECT_EQ(is->operands()[0]->type().id, TypeId::F32);
}

// ---- RC-FP-005: FSQRT / FRINTN/P/M/Z/A S and D ----

TEST(Translate, FSqrtS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E21C000u, code); // FSQRT s0, s0
    ExpectOneInstr(code, Opcode::Sqrt, 2u);
}

TEST(Translate, FSqrtD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E61C000u, code); // FSQRT d0, d0
    ExpectOneInstr(code, Opcode::Sqrt, 2u);
}

TEST(Translate, FFrintNS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E244000u, code); // FRINTN s0, s0
    ExpectOneInstr(code, Opcode::FrintN, 2u);
}

TEST(Translate, FFrintND) {
    sr::common::u8 code[4];
    WordToBytes(0x1E644000u, code); // FRINTN d0, d0
    ExpectOneInstr(code, Opcode::FrintN, 2u);
}

TEST(Translate, FFrintPS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E24C000u, code); // FRINTP s0, s0
    ExpectOneInstr(code, Opcode::FrintP, 2u);
}

TEST(Translate, FFrintPD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E64C000u, code); // FRINTP d0, d0
    ExpectOneInstr(code, Opcode::FrintP, 2u);
}

TEST(Translate, FFrintMS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E254000u, code); // FRINTM s0, s0
    ExpectOneInstr(code, Opcode::FrintM, 2u);
}

TEST(Translate, FFrintMD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E654000u, code); // FRINTM d0, d0
    ExpectOneInstr(code, Opcode::FrintM, 2u);
}

TEST(Translate, FFrintZS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E25C000u, code); // FRINTZ s0, s0
    ExpectOneInstr(code, Opcode::FrintZ, 2u);
}

TEST(Translate, FFrintZD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E65C000u, code); // FRINTZ d0, d0
    ExpectOneInstr(code, Opcode::FrintZ, 2u);
}

TEST(Translate, FFrintAS) {
    sr::common::u8 code[4];
    WordToBytes(0x1E264000u, code); // FRINTA s0, s0
    ExpectOneInstr(code, Opcode::FrintA, 2u);
}

TEST(Translate, FFrintAD) {
    sr::common::u8 code[4];
    WordToBytes(0x1E664000u, code); // FRINTA d0, d0
    ExpectOneInstr(code, Opcode::FrintA, 2u);
}

// Verify FSQRT D fields and F64 typing with distinct src/dst.
TEST(Translate, FSqrtDFields) {
    sr::common::u8 code[4];
    WordToBytes(0x1E61C062u, code); // FSQRT d2, d3
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    Instruction* sqrt =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(sqrt->opcode(), Opcode::Sqrt);
    EXPECT_EQ(sqrt->type().id, TypeId::F64);
    ASSERT_EQ(sqrt->operands().size(), 2u);
    EXPECT_EQ(sqrt->operands()[0]->type().id, TypeId::F64);
    EXPECT_EQ(sqrt->operands()[1]->type().id, TypeId::F64);
    EXPECT_EQ(dynamic_cast<Constant*>(sqrt->operands()[0])->value(), 2u);
    EXPECT_EQ(dynamic_cast<Constant*>(sqrt->operands()[1])->value(), 3u);
}

// Verify FRINTZ S fields and F32 typing with distinct src/dst.
TEST(Translate, FFrintZSFields) {
    sr::common::u8 code[4];
    WordToBytes(0x1E25C062u, code); // FRINTZ s2, s3
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    Instruction* frint =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(frint->opcode(), Opcode::FrintZ);
    EXPECT_EQ(frint->type().id, TypeId::F32);
    ASSERT_EQ(frint->operands().size(), 2u);
    EXPECT_EQ(frint->operands()[0]->type().id, TypeId::F32);
    EXPECT_EQ(frint->operands()[1]->type().id, TypeId::F32);
    EXPECT_EQ(dynamic_cast<Constant*>(frint->operands()[0])->value(), 2u);
    EXPECT_EQ(dynamic_cast<Constant*>(frint->operands()[1])->value(), 3u);
}

// ---- SIMD stage 2: MOVI/FDUP/INS/UMOV/FADD.4S ----

TEST(Translate, Movi4S) {
    sr::common::u8 code[4];
    WordToBytes(0x4F03F700u, code); // MOVI v0.4S, #1.5
    ExpectOneInstr(code, Opcode::Mov, 2u);
}

TEST(Translate, Movi4SFields) {
    // MOVI v1.4S, #2.5 (0x4F00F481): dst is V1 (V128), payload constant has
    // the f32 bits of 2.5 (0x40200000) duplicated into all four lanes.
    sr::common::u8 code[4];
    WordToBytes(0x4F00F481u, code);
    auto module = TranslateOne(code);
    ASSERT_NE(module, nullptr);
    Instruction* mov =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(mov->opcode(), Opcode::Mov);
    EXPECT_EQ(mov->type(), Type::V128(LaneKind::F32x4));
    ASSERT_EQ(mov->operands().size(), 2u);
    auto* dst = dynamic_cast<Constant*>(mov->operands()[0]);
    auto* src = dynamic_cast<Constant*>(mov->operands()[1]);
    ASSERT_NE(dst, nullptr);
    ASSERT_NE(src, nullptr);
    EXPECT_EQ(dst->type(), Type::V128(LaneKind::F32x4));
    EXPECT_EQ(dst->value(), 1u);
    EXPECT_EQ(src->type(), Type::V128(LaneKind::F32x4));
    const sr::common::u64 expect =
        0x40200000ull | (0x40200000ull << 32);
    EXPECT_EQ(src->value(), expect);
    EXPECT_EQ(src->value_hi(), expect);
}

TEST(Translate, Fdup4S) {
    sr::common::u8 code[4];
    WordToBytes(0x4E040400u, code); // FDUP v0.4S, s0
    ExpectOneInstr(code, Opcode::Broadcast, 2u);
}

TEST(Translate, Fdup4SFields) {
    // FDUP v1.4S, s5 (0x4E0404A1): dst v1, scalar src s5.
    sr::common::u8 code[4];
    WordToBytes(0x4E0404A1u, code);
    auto module = TranslateOne(code);
    Instruction* bcast =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(bcast->opcode(), Opcode::Broadcast);
    EXPECT_EQ(bcast->type(), Type::V128(LaneKind::F32x4));
    auto* dst = dynamic_cast<Constant*>(bcast->operands()[0]);
    auto* src = dynamic_cast<Constant*>(bcast->operands()[1]);
    ASSERT_NE(dst, nullptr);
    ASSERT_NE(src, nullptr);
    EXPECT_EQ(dst->value(), 1u);
    EXPECT_EQ(src->type().id, TypeId::F32);
    EXPECT_EQ(src->value(), 5u);
}

TEST(Translate, InsSLane2) {
    sr::common::u8 code[4];
    WordToBytes(0x6E140441u, code); // INS v1.S[2], v2.S[0]
    ExpectOneInstr(code, Opcode::InsertLane, 3u);
}

TEST(Translate, InsSLane3) {
    // INS v2.S[3], v4.S[0] (0x6E1C0482): lane constant must be 3.
    sr::common::u8 code[4];
    WordToBytes(0x6E1C0482u, code);
    auto module = TranslateOne(code);
    Instruction* ins =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(ins->opcode(), Opcode::InsertLane);
    ASSERT_EQ(ins->operands().size(), 3u);
    auto* dst = dynamic_cast<Constant*>(ins->operands()[0]);
    auto* lane = dynamic_cast<Constant*>(ins->operands()[1]);
    auto* src = dynamic_cast<Constant*>(ins->operands()[2]);
    ASSERT_NE(dst, nullptr);
    ASSERT_NE(lane, nullptr);
    ASSERT_NE(src, nullptr);
    EXPECT_EQ(dst->value(), 2u);
    EXPECT_EQ(lane->type().id, TypeId::U32);
    EXPECT_EQ(lane->value(), 3u);
    EXPECT_EQ(src->type().id, TypeId::F32);
    EXPECT_EQ(src->value(), 4u);
}

TEST(Translate, UmovSLane2) {
    sr::common::u8 code[4];
    WordToBytes(0x0E143C23u, code); // UMOV w3, v1.S[2]
    ExpectOneInstr(code, Opcode::ExtractLane, 3u);
}

TEST(Translate, UmovSFields) {
    // UMOV w3, v1.S[2]: dst x3, vector src v1, lane constant 2.
    sr::common::u8 code[4];
    WordToBytes(0x0E143C23u, code);
    auto module = TranslateOne(code);
    Instruction* umov =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(umov->opcode(), Opcode::ExtractLane);
    EXPECT_EQ(umov->type().id, TypeId::U32);
    ASSERT_EQ(umov->operands().size(), 3u);
    auto* dst = dynamic_cast<Constant*>(umov->operands()[0]);
    auto* src = dynamic_cast<Constant*>(umov->operands()[1]);
    auto* lane = dynamic_cast<Constant*>(umov->operands()[2]);
    ASSERT_NE(dst, nullptr);
    ASSERT_NE(src, nullptr);
    ASSERT_NE(lane, nullptr);
    EXPECT_EQ(dst->type().id, TypeId::U64);
    EXPECT_EQ(dst->value(), 3u);
    EXPECT_EQ(src->type(), Type::V128(LaneKind::F32x4));
    EXPECT_EQ(src->value(), 1u);
    EXPECT_EQ(lane->value(), 2u);
}

TEST(Translate, Fadd4S) {
    sr::common::u8 code[4];
    WordToBytes(0x4E21D402u, code); // FADD v2.4S, v0.4S, v1.4S
    ExpectOneInstr(code, Opcode::Add, 3u);
}

TEST(Translate, Fadd4SFields) {
    sr::common::u8 code[4];
    WordToBytes(0x4E21D402u, code);
    auto module = TranslateOne(code);
    Instruction* add =
        module->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(add->opcode(), Opcode::Add);
    EXPECT_EQ(add->type(), Type::V128(LaneKind::F32x4));
    ASSERT_EQ(add->operands().size(), 3u);
    for (Value* op : add->operands()) {
        EXPECT_EQ(op->type(), Type::V128(LaneKind::F32x4));
    }
    EXPECT_EQ(dynamic_cast<Constant*>(add->operands()[0])->value(), 2u);
    EXPECT_EQ(dynamic_cast<Constant*>(add->operands()[1])->value(), 0u);
    EXPECT_EQ(dynamic_cast<Constant*>(add->operands()[2])->value(), 1u);
}
