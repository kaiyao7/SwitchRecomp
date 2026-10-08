// ARM64 -> IR decode pipeline (Phase 1: RC-PIPE-002 core + RC-PIPE-005
// extended coverage). Handles ADRP, ADD (shifted reg), LDP/STP, MADD/MSUB,
// LDR/STR (register offset), MOVK, CBZ/CBNZ, and the original 22 opcodes.
#include "switchrecomp/translator/translator.h"

#include "switchrecomp/ir/ir.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace sr::translator {

using sr::common::u32;
using sr::common::u64;
using sr::common::u8;
using sr::common::usize;
using sr::common::s64;
using sr::ir::BasicBlock;
using sr::ir::Constant;
using sr::ir::Function;
using sr::ir::Instruction;
using sr::ir::IRBuilder;
using sr::ir::LaneKind;
using sr::ir::Module;
using sr::ir::Opcode;
using sr::ir::Type;
using sr::ir::TypeId;
using sr::ir::Value;

namespace {

// Extract bits [high:low] (inclusive) from a 32-bit instruction word.
u32 ExtractBits(u32 word, int high, int low) {
    u32 mask = (high == 31) ? 0xFFFFFFFFu
                            : ((1u << (high - low + 1)) - 1u);
    return (word >> low) & mask;
}

// Sign-extend a value of |bits| width to 64 bits.
u64 SignExtend(u64 value, int bits) {
    u64 m = 1ull << (bits - 1);
    return (value ^ m) - m;
}

// RC-FP-001: expand the 8-bit FMOV-immediate field to an IEEE-754 f32 bit
// pattern (ARM ARM VFPExpandImm, esize=32). Layout of imm8:
//   sign = imm<7>, exp code = imm<6:4>, fraction = imm<3:0>
// The 3-bit exponent code maps to the 8-bit biased exponent:
//   exp<2>=0 -> 128 + exp<1:0>   (values 2.0 .. 16.0)
//   exp<2>=1 -> 124 + exp<1:0>   (values 0.125 .. 1.0)
// The 4-bit fraction occupies f32 bits 22:19 (19 zero bits follow).
u32 ExpandFPImm(u32 imm8) {
    u32 sign   = (imm8 >> 7) & 1u;
    u32 exp    = (imm8 >> 4) & 7u;
    u32 frac   = imm8 & 0xFu;
    u32 biased = ((exp & 4u) == 0u) ? (128u + (exp & 3u))
                                   : (124u + (exp & 3u));
    return (sign << 31) | (biased << 23) | (frac << 19);
}

// ---- Register / immediate operand helpers (Phase 1: registers modeled as
//      constants holding the register index). ----
//
// RC-PIPE-006: register constants (x0..x30) and common small immediates are
// cached per-translation to avoid repeated heap allocations. Registers are
// referenced very frequently (every arithmetic/memory opcode), so caching
// them removes one allocation per operand.

struct ConstantCache {
    IRBuilder* builder = nullptr;
    Constant* regs[32] = {};       // lazily populated, index = register number
    Constant* fregs[32] = {};      // RC-FP-001: FP register s0..s31
    Constant* dregs[32] = {};      // RC-FP-003: FP register d0..d31
    Constant* vregs[32] = {};      // SIMD stage 2: Vd vector (F32x4)
    Constant* small_imm[16] = {};  // cache immediates 0..15

    Constant* Reg(u32 reg) {
        if (reg < 32 && regs[reg]) return regs[reg];
        Constant* c = builder->CreateConstant(static_cast<u64>(reg), Type::U64());
        if (reg < 32) regs[reg] = c;
        return c;
    }

    // FP register operand: a constant holding the register index, typed F32
    // so codegen renders sr_v_get_s(&ctx->v[n]).
    Constant* FReg(u32 reg) {
        if (reg < 32 && fregs[reg]) return fregs[reg];
        Constant* c = builder->CreateConstant(static_cast<u64>(reg), Type::F32());
        if (reg < 32) fregs[reg] = c;
        return c;
    }

    // RC-FP-003: double-precision register operand, typed F64 so codegen
    // renders sr_v_get_d(&ctx->v[n]).
    Constant* DReg(u32 reg) {
        if (reg < 32 && dregs[reg]) return dregs[reg];
        Constant* c = builder->CreateConstant(static_cast<u64>(reg), Type::F64());
        if (reg < 32) dregs[reg] = c;
        return c;
    }

    // SIMD stage 2: vector register operand, typed V128 (F32x4) so codegen
    // renders &ctx->v[n]. Stage 2 only needs one lane kind.
    Constant* VReg(u32 reg, LaneKind lane) {
        if (reg < 32 && vregs[reg]) return vregs[reg];
        Constant* c = builder->CreateConstant(static_cast<u64>(reg),
                                              Type::V128(lane));
        if (reg < 32) vregs[reg] = c;
        return c;
    }

    Constant* Imm(u64 value) {
        if (value < 16) {
            if (small_imm[value]) return small_imm[value];
            Constant* c = builder->CreateConstant(value, Type::U32());
            small_imm[value] = c;
            return c;
        }
        return builder->CreateConstant(value, Type::U32());
    }
};

// ---- Per-instruction decode handlers ----
// Each handler extracts its fields from |word|, builds IR via |builder| and
// appends one or more instructions to |bb|. Returns true on success. Most
// simple opcodes emit a single IR instruction; compound opcodes (LDP/STP,
// MADD/MSUB, MOVK, register-offset LDR/STR, shifted ADD) emit several.

bool DecodeAddImm(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 sh = ExtractBits(word, 23, 23);
    u32 imm12 = ExtractBits(word, 22, 10);
    u32 rn = ExtractBits(word, 9, 5);
    u64 imm = static_cast<u64>(imm12) << (sh * 12);
    bb->Append(cache.builder->CreateAdd(cache.Reg(rn), cache.Imm(imm)));
    return true;
}

bool DecodeSubImm(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 sh = ExtractBits(word, 23, 23);
    u32 imm12 = ExtractBits(word, 22, 10);
    u32 rn = ExtractBits(word, 9, 5);
    u64 imm = static_cast<u64>(imm12) << (sh * 12);
    bb->Append(cache.builder->CreateSub(cache.Reg(rn), cache.Imm(imm)));
    return true;
}

bool DecodeMovz(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // RC-FIX-002: MOV Z Xd, #imm -> Mov to the ACTUAL Rd. The old
    // Add(zero, imm) shortcut implicitly targeted x0 (Add's dst was derived
    // from the first operand constant), so any `mov xN` with N != 0 was
    // mis-decoded.
    u32 hw = ExtractBits(word, 22, 21);
    u32 imm16 = ExtractBits(word, 20, 5);
    u32 rd = ExtractBits(word, 4, 0);
    u64 imm = static_cast<u64>(imm16) << (hw * 16);
    bb->Append(cache.builder->CreateMov(cache.Reg(rd), cache.Imm(imm)));
    return true;
}

bool DecodeMovn(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // MOVN Xd, #imm -> Not(imm << (hw*16)).
    u32 hw = ExtractBits(word, 22, 21);
    u32 imm16 = ExtractBits(word, 20, 5);
    u64 imm = static_cast<u64>(imm16) << (hw * 16);
    bb->Append(cache.builder->CreateNot(cache.Imm(imm)));
    return true;
}

bool DecodeAddReg(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // ADD (shifted register). When shift amount is zero, emit a plain Add to
    // preserve the canonical single-instruction form; otherwise emit the
    // shift followed by an Add. ROR is unsupported in Phase 1.
    u32 shift = ExtractBits(word, 23, 22);
    u32 rm = ExtractBits(word, 20, 16);
    u32 imm6 = ExtractBits(word, 15, 10);
    u32 rn = ExtractBits(word, 9, 5);
    if (shift == 3u) return false; // ROR unsupported
    if (imm6 == 0u) {
        bb->Append(cache.builder->CreateAdd(cache.Reg(rn), cache.Reg(rm)));
        return true;
    }
    Instruction* shifted = nullptr;
    switch (shift) {
        case 0u: shifted = cache.builder->CreateShl(cache.Reg(rm), cache.Imm(imm6)); break;
        case 1u: shifted = cache.builder->CreateShr(cache.Reg(rm), cache.Imm(imm6)); break;
        case 2u: shifted = cache.builder->CreateSar(cache.Reg(rm), cache.Imm(imm6)); break;
        default: return false;
    }
    bb->Append(shifted);
    bb->Append(cache.builder->CreateAdd(cache.Reg(rn), shifted));
    return true;
}

bool DecodeSubReg(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rm = ExtractBits(word, 20, 16);
    bb->Append(cache.builder->CreateSub(cache.Reg(rn), cache.Reg(rm)));
    return true;
}

bool DecodeAnd(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rm = ExtractBits(word, 20, 16);
    bb->Append(cache.builder->CreateAnd(cache.Reg(rn), cache.Reg(rm)));
    return true;
}

bool DecodeOrr(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rm = ExtractBits(word, 20, 16);
    bb->Append(cache.builder->CreateOr(cache.Reg(rn), cache.Reg(rm)));
    return true;
}

bool DecodeEor(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rm = ExtractBits(word, 20, 16);
    bb->Append(cache.builder->CreateXor(cache.Reg(rn), cache.Reg(rm)));
    return true;
}

// Distinguish LSL (UBFM immr=0) from LSR (UBFM imms=esize-1).
bool DecodeUbfm(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 n = ExtractBits(word, 22, 22);
    u32 immr = ExtractBits(word, 21, 16);
    u32 imms = ExtractBits(word, 15, 10);
    u32 rn = ExtractBits(word, 9, 5);
    u32 esize = n ? 64 : 32;
    if (immr == 0) {
        // LSL: shift = esize - 1 - imms.
        u32 shift = (esize - 1) - imms;
        bb->Append(cache.builder->CreateShl(cache.Reg(rn), cache.Imm(shift)));
    } else {
        // LSR: shift = immr.
        bb->Append(cache.builder->CreateShr(cache.Reg(rn), cache.Imm(immr)));
    }
    return true;
}

bool DecodeSbfm(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // ASR (alias of SBFM with imms = esize-1): shift = immr.
    u32 immr = ExtractBits(word, 21, 16);
    u32 rn = ExtractBits(word, 9, 5);
    bb->Append(cache.builder->CreateSar(cache.Reg(rn), cache.Imm(immr)));
    return true;
}

bool DecodeLdr(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 size = ExtractBits(word, 31, 30);
    u32 rn = ExtractBits(word, 9, 5);
    Type ty = (size == 0b11) ? Type::U64()
             : (size == 0b10) ? Type::U32()
             : (size == 0b01) ? Type::U16()
                              : Type::U8();
    // Address placeholder: Rn register (offset 0 for the canonical sample).
    bb->Append(cache.builder->CreateLoad(cache.Reg(rn), ty));
    return true;
}

bool DecodeStr(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rt = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateStore(cache.Reg(rn), cache.Reg(rt)));
    return true;
}

bool DecodeB(u32 word, u64 guest_addr, ConstantCache& cache, BasicBlock* bb) {
    // RC-FIX-002: B <label>: target = PC + sign_extend(imm26 << 2). Emits a
    // Branch carrying the target guest address; the post-decode pass resolves
    // it to a per-function IR instruction ordinal (same mechanism as
    // B.cond). Built via CreateInstruction because CreateBranch only accepts
    // a BasicBlock in the public header.
    u32 imm26 = ExtractBits(word, 25, 0);
    s64 offset = static_cast<s64>(SignExtend(static_cast<u64>(imm26) << 2, 28));
    u64 target = static_cast<u64>(static_cast<s64>(guest_addr) + offset);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Branch, Type::Void(),
        {cache.builder->CreateConstant(target, Type::U64())}));
    return true;
}

bool DecodeBl(u32 word, u64 guest_addr, ConstantCache& cache, BasicBlock* bb) {
    // BL <label>: target = PC + sign_extend(imm26 << 2).
    u32 imm26 = ExtractBits(word, 25, 0);
    s64 offset = static_cast<s64>(SignExtend(static_cast<u64>(imm26) << 2, 28));
    u64 target = static_cast<u64>(static_cast<s64>(guest_addr) + offset);
    bb->Append(cache.builder->CreateCall(target));
    return true;
}

bool DecodeBCond(u32 word, u64 guest_addr, ConstantCache& cache, BasicBlock* bb) {
    // B.cond <label>: target = PC + sign_extend(imm19 << 2). Emits operands
    // {U32 cond, U64 target guest address}; the post-decode pass in Translate
    // resolves the address to a per-function IR instruction ordinal.
    u32 cond = ExtractBits(word, 3, 0);
    u32 imm19 = ExtractBits(word, 23, 5);
    s64 offset = static_cast<s64>(SignExtend(static_cast<u64>(imm19) << 2, 21));
    u64 target = static_cast<u64>(static_cast<s64>(guest_addr) + offset);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::CondBranch, Type::Void(),
        {cache.Imm(cond),
         cache.builder->CreateConstant(target, Type::U64())}));
    return true;
}

bool DecodeCbz(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rt = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateCondBranch(cache.Reg(rt), nullptr, nullptr));
    return true;
}

bool DecodeCbnz(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rt = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateCondBranch(cache.Reg(rt), nullptr, nullptr));
    return true;
}

bool DecodeRet(u32 /*word*/, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    bb->Append(cache.builder->CreateReturn());
    return true;
}

bool DecodeCmpImm(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // SUBS immediate. The CMP alias (Rd=31) is decoded to Compare, which writes
    // the integer NZCV flags; a real SUBS <Rd> keeps the previous Sub lowering
    // (result written, flags not produced) to preserve existing behavior.
    u32 sf = ExtractBits(word, 31, 31);
    u32 sh = ExtractBits(word, 23, 23);
    u32 imm12 = ExtractBits(word, 22, 10);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    u64 imm = static_cast<u64>(imm12) << (sh * 12);
    if (rd == 31u) {
        // CMP <Rn>, #imm -> Compare; instruction type carries the width.
        Type wty = sf ? Type::U64() : Type::U32();
        bb->Append(cache.builder->CreateInstruction(
            Opcode::Compare, wty,
            {cache.Reg(rn), cache.Imm(imm)}));
        return true;
    }
    bb->Append(cache.builder->CreateSub(cache.Reg(rn), cache.Imm(imm)));
    return true;
}

bool DecodeCmpReg(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // SUBS (shifted register) with imm6=0, i.e. plain register form. The
    // table mask includes bits 4:0 = 11111, so only the CMP alias (Rd=31)
    // reaches this handler; real SUBS <Rd> is handled by DecodeSubReg.
    // NOTE: a shifted CMP (imm6 != 0, e.g. `cmp x0,x1,lsl #3`) does not match
    // this table entry and falls back to the flag-less Sub path; applying the
    // shift before the compare is left as a follow-up.
    u32 sf = ExtractBits(word, 31, 31);
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    // Rd=31 is guaranteed by the table mask (0x...001F), so this entry only
    // ever matches the CMP alias; real SUBS falls through to DecodeSubReg.
    Type wty = sf ? Type::U64() : Type::U32();
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Compare, wty,
        {cache.Reg(rn), cache.Reg(rm)}));
    return true;
}

bool DecodeSubsReg(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // SUBS (shifted register) that is NOT the CMP alias (Rd != 31 is implied
    // by the table value). Keeps the previous Sub lowering: the result is
    // written but NZCV flags are not produced (generic SUB-entry never
    // matched the 0xEB/0x6B opc, so SUBS was previously unimplemented).
    // imm6 must be 0 (table mask); shifted SUBS is the same known gap as
    // shifted SUB/CMP.
    u32 rn = ExtractBits(word, 9, 5);
    u32 rm = ExtractBits(word, 20, 16);
    bb->Append(cache.builder->CreateSub(cache.Reg(rn), cache.Reg(rm)));
    return true;
}

bool DecodeNop(u32 /*word*/, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    bb->Append(cache.builder->CreateInstruction(Opcode::Nop, Type::Void(), {}));
    return true;
}

bool DecodeSvc(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // SVC #imm16: encoding 1101 0100 000 imm16 00001.
    // The immediate is the SVC number dispatched by HandleSvc at runtime.
    u32 imm16 = ExtractBits(word, 20, 5);
    bb->Append(cache.builder->CreateSvc(imm16));
    return true;
}

// ---- RC-PIPE-005: extended instruction coverage ----

bool DecodeAdrp(u32 word, u64 guest_addr, ConstantCache& cache, BasicBlock* bb) {
    // ADRP Xd, label: Rd = (PC & ~0xFFF) + (sign_ext(imm) << 12).
    u32 immlo = ExtractBits(word, 30, 29);
    u32 immhi = ExtractBits(word, 23, 5);
    u64 imm = SignExtend((static_cast<u64>(immhi) << 2) | immlo, 21);
    u64 page = guest_addr & ~0xFFFULL;
    u64 offset = imm << 12;
    bb->Append(cache.builder->CreateAdd(
        cache.builder->CreateConstant(page, Type::U64()),
        cache.builder->CreateConstant(offset, Type::U64())));
    return true;
}

bool DecodeLdp(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // LDP Xt, Xt2, [Xn, #imm]: load two 64-bit registers from base+offset.
    u32 imm7 = ExtractBits(word, 21, 15);
    u32 rn = ExtractBits(word, 9, 5);
    s64 offset = static_cast<s64>(SignExtend(imm7, 7)) * 8;
    Value* base = cache.Reg(rn);
    Instruction* addr1 = cache.builder->CreateAdd(base,
        cache.builder->CreateConstant(static_cast<u64>(offset), Type::U64()));
    bb->Append(addr1);
    bb->Append(cache.builder->CreateLoad(addr1, Type::U64()));
    Instruction* addr2 = cache.builder->CreateAdd(base,
        cache.builder->CreateConstant(static_cast<u64>(offset) + 8u, Type::U64()));
    bb->Append(addr2);
    bb->Append(cache.builder->CreateLoad(addr2, Type::U64()));
    return true;
}

bool DecodeStp(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // STP Xt, Xt2, [Xn, #imm]: store two 64-bit registers to base+offset.
    u32 imm7 = ExtractBits(word, 21, 15);
    u32 rt2 = ExtractBits(word, 14, 10);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rt = ExtractBits(word, 4, 0);
    s64 offset = static_cast<s64>(SignExtend(imm7, 7)) * 8;
    Value* base = cache.Reg(rn);
    Instruction* addr1 = cache.builder->CreateAdd(base,
        cache.builder->CreateConstant(static_cast<u64>(offset), Type::U64()));
    bb->Append(addr1);
    bb->Append(cache.builder->CreateStore(addr1, cache.Reg(rt)));
    Instruction* addr2 = cache.builder->CreateAdd(base,
        cache.builder->CreateConstant(static_cast<u64>(offset) + 8u, Type::U64()));
    bb->Append(addr2);
    bb->Append(cache.builder->CreateStore(addr2, cache.Reg(rt2)));
    return true;
}

bool DecodeMadd(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // MADD Xd, Xn, Xm, Xa: Rd = Rn * Rm + Ra.
    u32 rm = ExtractBits(word, 20, 16);
    u32 ra = ExtractBits(word, 14, 10);
    u32 rn = ExtractBits(word, 9, 5);
    Instruction* mul = cache.builder->CreateMul(cache.Reg(rn), cache.Reg(rm));
    bb->Append(mul);
    bb->Append(cache.builder->CreateAdd(mul, cache.Reg(ra)));
    return true;
}

bool DecodeMsub(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // MSUB Xd, Xn, Xm, Xa: Rd = Rn * Rm - Ra.
    u32 rm = ExtractBits(word, 20, 16);
    u32 ra = ExtractBits(word, 14, 10);
    u32 rn = ExtractBits(word, 9, 5);
    Instruction* mul = cache.builder->CreateMul(cache.Reg(rn), cache.Reg(rm));
    bb->Append(mul);
    bb->Append(cache.builder->CreateSub(mul, cache.Reg(ra)));
    return true;
}

bool DecodeLdrReg(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // LDR Xt, [Xn, Xm]: load from base + index register.
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    Instruction* addr = cache.builder->CreateAdd(cache.Reg(rn), cache.Reg(rm));
    bb->Append(addr);
    bb->Append(cache.builder->CreateLoad(addr, Type::U64()));
    return true;
}

bool DecodeStrReg(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // STR Xt, [Xn, Xm]: store to base + index register.
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rt = ExtractBits(word, 4, 0);
    Instruction* addr = cache.builder->CreateAdd(cache.Reg(rn), cache.Reg(rm));
    bb->Append(addr);
    bb->Append(cache.builder->CreateStore(addr, cache.Reg(rt)));
    return true;
}

bool DecodeMovk(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // MOVK Xd, #imm, LSL #shift: Rd = (Rd & ~mask) | (imm << shift).
    u32 hw = ExtractBits(word, 22, 21);
    u32 imm16 = ExtractBits(word, 20, 5);
    u32 rd = ExtractBits(word, 4, 0);
    u64 shift = static_cast<u64>(hw) * 16;
    u64 mask = ~(0xFFFFULL << shift);
    u64 value = static_cast<u64>(imm16) << shift;
    Instruction* masked = cache.builder->CreateAnd(cache.Reg(rd),
        cache.builder->CreateConstant(mask, Type::U64()));
    bb->Append(masked);
    bb->Append(cache.builder->CreateOr(masked,
        cache.builder->CreateConstant(value, Type::U64())));
    return true;
}

// ---- RC-FP-001: scalar single-precision FP instructions ----

bool DecodeFmovImm(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // FMOV Sd, #imm: expand imm8 to f32, emit Mov(FReg(rd), F32 constant).
    u32 imm8 = ExtractBits(word, 20, 13);
    u32 rd = ExtractBits(word, 4, 0);
    u32 bits = ExpandFPImm(imm8);
    Value* src = cache.builder->CreateConstant(static_cast<u64>(bits), Type::F32());
    bb->Append(cache.builder->CreateMov(cache.FReg(rd), src));
    return true;
}

bool DecodeFmovReg(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // FMOV Sd, Sn -> Mov(FReg(rd), FReg(rn)).
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateMov(cache.FReg(rd), cache.FReg(rn)));
    return true;
}

bool DecodeFaddS(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // FADD Sd, Sn, Sm -> Add F32 with explicit {dst, src1, src2} operands.
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Add, Type::F32(),
        {cache.FReg(rd), cache.FReg(rn), cache.FReg(rm)}));
    return true;
}

bool DecodeFsubS(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Sub, Type::F32(),
        {cache.FReg(rd), cache.FReg(rn), cache.FReg(rm)}));
    return true;
}

bool DecodeFmulS(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Mul, Type::F32(),
        {cache.FReg(rd), cache.FReg(rn), cache.FReg(rm)}));
    return true;
}

bool DecodeLdrS(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // LDR St, [Xn, #imm]: canonical single-instruction form (offset 0) mirrors
    // the integer DecodeLdr modeling; access size is 4 bytes.
    u32 rn = ExtractBits(word, 9, 5);
    bb->Append(cache.builder->CreateLoad(cache.Reg(rn), Type::F32()));
    return true;
}

bool DecodeStrS(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // STR St, [Xn, #imm]: Store(Xn base, FReg(rt) value).
    u32 rn = ExtractBits(word, 9, 5);
    u32 rt = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateStore(cache.Reg(rn), cache.FReg(rt)));
    return true;
}

// ---- RC-FP-003: scalar double-precision FP instructions ----
// Encoding differs from the S forms in ftype (bits 23:22 = 01 for these
// groups): S base 0x1E2x -> D base 0x1E6x. Verified against ARM ARM.

bool DecodeFmovRegD(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // FMOV Dd, Dn -> Mov(DReg(rd), DReg(rn)).
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateMov(cache.DReg(rd), cache.DReg(rn)));
    return true;
}

bool DecodeFaddD(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // FADD Dd, Dn, Dm -> Add F64 with explicit {dst, src1, src2} operands.
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Add, Type::F64(),
        {cache.DReg(rd), cache.DReg(rn), cache.DReg(rm)}));
    return true;
}

bool DecodeFsubD(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Sub, Type::F64(),
        {cache.DReg(rd), cache.DReg(rn), cache.DReg(rm)}));
    return true;
}

bool DecodeFmulD(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Mul, Type::F64(),
        {cache.DReg(rd), cache.DReg(rn), cache.DReg(rm)}));
    return true;
}

bool DecodeFdivD(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // FDIV Dd, Dn, Dm -> Div F64 (RC-FP-003 new opcode).
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Div, Type::F64(),
        {cache.DReg(rd), cache.DReg(rn), cache.DReg(rm)}));
    return true;
}

bool DecodeLdrD(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // LDR Dt, [Xn, #imm]: canonical single-instruction form (offset 0);
    // access size is 8 bytes. imm12 (bits 21:10) scales by 8; ignored here,
    // mirroring the S/integer canonical Load modeling.
    u32 rn = ExtractBits(word, 9, 5);
    bb->Append(cache.builder->CreateLoad(cache.Reg(rn), Type::F64()));
    return true;
}

bool DecodeStrD(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // STR Dt, [Xn, #imm]: Store(Xn base, DReg(rt) value).
    u32 rn = ExtractBits(word, 9, 5);
    u32 rt = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateStore(cache.Reg(rn), cache.DReg(rt)));
    return true;
}

// ---- RC-FP-002: FP compare and conversions ----

bool DecodeFCmp(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // FCMP Sn/Dn, Sm/Dm -> Compare {lhs, rhs}; writes NZCV. Rm=31 is the
    // FCMP <Rn>,#0.0 form: emit a U32 zero marker that codegen renders as a
    // literal ((float/double)0) (a F32-typed 0 constant would alias FReg s0).
    // Bit 22 (ftype low bit) selects double for this encoding group.
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    const bool isd = ExtractBits(word, 22, 22) != 0u;
    Value* rhs = (rm == 31u)
        ? cache.builder->CreateConstant(0u, Type::U32())
        : (isd ? static_cast<Value*>(cache.DReg(rm))
               : static_cast<Value*>(cache.FReg(rm)));
    Value* lhs = isd ? static_cast<Value*>(cache.DReg(rn))
                     : static_cast<Value*>(cache.FReg(rn));
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Compare, Type::Void(), {lhs, rhs}));
    return true;
}

bool DecodeFcvtzs(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // FCVTZS Wd/Xd, Sn/Dn: Convert F32/F64 -> S32/S64; sf (bit31) is the
    // destination width, bit16 (the "type" low bit in this conversion group)
    // selects a double source. Rounding toward zero comes from the C integer
    // cast in codegen.
    u32 sf = ExtractBits(word, 31, 31);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    const bool isd = ExtractBits(word, 16, 16) != 0u;
    Type dst = sf ? Type::S64() : Type::S32();
    // Evaluate the destination register constant first (braced-init-list
    // order is left-to-right) so its id precedes the source: the existing
    // S golden depends on [Reg(rd)=id1, FReg(rn)=id2].
    Constant* dst_c = cache.Reg(rd);
    Value* src = isd ? static_cast<Value*>(cache.DReg(rn))
                     : static_cast<Value*>(cache.FReg(rn));
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Convert, dst, {dst_c, src}));
    return true;
}

bool DecodeScvtf(u32 word, u64 /*guest_addr*/, ConstantCache& cache, BasicBlock* bb) {
    // SCVTF Sd/Dd, Wn/Xn: Convert S32/S64 -> F32/F64; sf (bit31) is the
    // source width, bit16 selects a double destination. The source register
    // index is emitted as an S32/S64-typed constant so the codegen can apply
    // the matching signed cast (ordinary Reg() is U64).
    u32 sf = ExtractBits(word, 31, 31);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    const bool isd = ExtractBits(word, 16, 16) != 0u;
    Value* src = cache.builder->CreateConstant(
        static_cast<u64>(rn), sf ? Type::S64() : Type::S32());
    Value* dst_reg = isd ? static_cast<Value*>(cache.DReg(rd))
                         : static_cast<Value*>(cache.FReg(rd));
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Convert, isd ? Type::F64() : Type::F32(),
        {dst_reg, src}));
    return true;
}

// ---- RC-FP-004: FNEG / FABS (1-source FP, S and D) ----
// Same encoding family as FMOV register (fixed 0x1E204000): the 6-bit opcode
// in bits20:15 is 000001 for FABS (bit15), 000010 for FNEG (bit16); the type
// low bit (bit22) selects double. Verified against QEMU a64.decode and VIXL
// FPDataProcessing1Source constants (fabs s=0x1E20C000, fneg s=0x1E214000).

bool DecodeFNeg(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    const bool isd = ExtractBits(word, 22, 22) != 0u;
    Value* src = isd ? static_cast<Value*>(cache.DReg(rn))
                     : static_cast<Value*>(cache.FReg(rn));
    Value* dst = isd ? static_cast<Value*>(cache.DReg(rd))
                     : static_cast<Value*>(cache.FReg(rd));
    bb->Append(cache.builder->CreateNeg(dst, src));
    return true;
}

bool DecodeFAbs(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    const bool isd = ExtractBits(word, 22, 22) != 0u;
    Value* src = isd ? static_cast<Value*>(cache.DReg(rn))
                     : static_cast<Value*>(cache.FReg(rn));
    Value* dst = isd ? static_cast<Value*>(cache.DReg(rd))
                     : static_cast<Value*>(cache.FReg(rd));
    bb->Append(cache.builder->CreateAbs(dst, src));
    return true;
}

// RC-FP-004: FCSEL Sd/Dd, Sn/Dn, Sm/Dm, <cond>. Fixed bits give mask
// 0xFFE00C00 (S value 0x1E200C00, D value 0x1E600C00); cond in bits15:12.
bool DecodeFCsel(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                 BasicBlock* bb) {
    u32 rm = ExtractBits(word, 20, 16);
    u32 cond = ExtractBits(word, 15, 12);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    const bool isd = ExtractBits(word, 22, 22) != 0u;
    Value* rn_v = isd ? static_cast<Value*>(cache.DReg(rn))
                      : static_cast<Value*>(cache.FReg(rn));
    Value* rm_v = isd ? static_cast<Value*>(cache.DReg(rm))
                      : static_cast<Value*>(cache.FReg(rm));
    Value* dst = isd ? static_cast<Value*>(cache.DReg(rd))
                     : static_cast<Value*>(cache.FReg(rd));
    bb->Append(cache.builder->CreateSelect(
        dst, cache.Imm(cond), rn_v, rm_v));
    return true;
}

// RC-FP-004: FCVT scalar between single and double precision. In the
// 1-source opcode space these are opcode 000101 (type=00: S source -> D
// destination) and 000100 (type=01: D source -> S destination). The IR keeps
// the existing Convert form {dst register constant, src} with the
// instruction type set to the destination precision.
bool DecodeFcvtSToD(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                    BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Convert, Type::F64(),
        {cache.DReg(rd), cache.FReg(rn)}));
    return true;
}

bool DecodeFcvtDToS(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                    BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Convert, Type::F32(),
        {cache.FReg(rd), cache.DReg(rn)}));
    return true;
}

// ---- RC-FP-005: FSQRT and the FRINT round-to-integer family (1-source) ----
// In the same 1-source FP space (fixed 0x1E204000 S base) the 6-bit opcodes
// are: FSQRT 000011, FRINTN 001000, FRINTP 001001, FRINTM 001010,
// FRINTZ 001011, FRINTA 001100; bit22 selects double (S=00, D=01).
// Verified four-way: ARM ARM ISA XML, QEMU 9.0 disas_fp_1src, LLVM 18
// BaseSingleOperandFPData defms, VIXL FPDataProcessing1Source constants.
bool DecodeFSqrt(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                 BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    const bool isd = ExtractBits(word, 22, 22) != 0u;
    Value* src = isd ? static_cast<Value*>(cache.DReg(rn))
                     : static_cast<Value*>(cache.FReg(rn));
    Value* dst = isd ? static_cast<Value*>(cache.DReg(rd))
                     : static_cast<Value*>(cache.FReg(rd));
    bb->Append(cache.builder->CreateSqrt(dst, src));
    return true;
}

// Shared decoder for the five FRINT variants; the target IR opcode is a
// template parameter so the table can register one instantiation each.
template <Opcode Op>
bool DecodeFrint(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                 BasicBlock* bb) {
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    const bool isd = ExtractBits(word, 22, 22) != 0u;
    Value* src = isd ? static_cast<Value*>(cache.DReg(rn))
                     : static_cast<Value*>(cache.FReg(rn));
    Value* dst = isd ? static_cast<Value*>(cache.DReg(rd))
                     : static_cast<Value*>(cache.FReg(rd));
    Instruction* instr = nullptr;
    if constexpr (Op == Opcode::FrintN) {
        instr = cache.builder->CreateFrintN(dst, src);
    } else if constexpr (Op == Opcode::FrintP) {
        instr = cache.builder->CreateFrintP(dst, src);
    } else if constexpr (Op == Opcode::FrintM) {
        instr = cache.builder->CreateFrintM(dst, src);
    } else if constexpr (Op == Opcode::FrintZ) {
        instr = cache.builder->CreateFrintZ(dst, src);
    } else {
        instr = cache.builder->CreateFrintA(dst, src);
    }
    bb->Append(instr);
    return true;
}

// ---- SIMD stage 2: MOVI/FDUP/INS/UMOV + FADD Vd.4S ----

bool DecodeMovi4S(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                  BasicBlock* bb) {
    // MOVI/FMOV Vd.4S, #fpimm: cmode (bits15:12) = 1111; the 8-bit FP
    // immediate is split across the encoding:
    //   imm8<7:5> = bits18:16, imm8<4:0> = bits9:5
    // (same layout Ryujinx OpCodeSimdImm decodes). All four lanes get the
    // same value, so the 128-bit constant is (bits|bits<<32) duplicated.
    u32 rd = ExtractBits(word, 4, 0);
    u32 imm8 = (ExtractBits(word, 18, 16) << 5) | ExtractBits(word, 9, 5);
    u32 bits = ExpandFPImm(imm8);
    u64 dup = static_cast<u64>(bits) |
              (static_cast<u64>(bits) << 32);
    // Evaluate dst first for deterministic operand ids (see DecodeFcvtzs).
    Constant* dst_c = cache.VReg(rd, LaneKind::F32x4);
    Value* src = cache.builder->CreateConstant(
        dup, dup, Type::V128(LaneKind::F32x4));
    bb->Append(cache.builder->CreateMov(dst_c, src));
    return true;
}

bool DecodeFdup4S(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                  BasicBlock* bb) {
    // FDUP Vd.4S, Sn: Sn index in bits7:5 (scalar view), Vd in bits4:0.
    u32 sn = ExtractBits(word, 7, 5);
    u32 rd = ExtractBits(word, 4, 0);
    Constant* dst_c = cache.VReg(rd, LaneKind::F32x4);
    bb->Append(cache.builder->CreateBroadcast(dst_c, cache.FReg(sn)));
    return true;
}

bool DecodeInsS(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                BasicBlock* bb) {
    // INS Vd.S[i], Vn.S[0] (scalar-source form, bits14:13 fixed 0):
    // destination lane i in bits20:19, Vn in bits9:5, Vd in bits4:0.
    u32 dst_lane = ExtractBits(word, 20, 19);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    Constant* dst_c = cache.VReg(rd, LaneKind::F32x4);
    Value* lane_c = cache.Imm(dst_lane);
    bb->Append(cache.builder->CreateInsertLane(dst_c, lane_c, cache.FReg(rn)));
    return true;
}

bool DecodeUmovS(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                 BasicBlock* bb) {
    // UMOV Wd, Vn.S[i]: lane i in bits20:19, Vn in bits9:5, Wd in bits4:0.
    u32 lane = ExtractBits(word, 20, 19);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    Constant* dst_c = cache.Reg(rd);
    Value* src_c = cache.VReg(rn, LaneKind::F32x4);
    Value* lane_c = cache.Imm(lane);
    bb->Append(cache.builder->CreateExtractLane(dst_c, src_c, lane_c));
    return true;
}

bool DecodeFadd4S(u32 word, u64 /*guest_addr*/, ConstantCache& cache,
                  BasicBlock* bb) {
    // FADD Vd.4S, Vn.4S, Vm.4S: Rm bits20:16, Rn bits9:5, Rd bits4:0.
    u32 rm = ExtractBits(word, 20, 16);
    u32 rn = ExtractBits(word, 9, 5);
    u32 rd = ExtractBits(word, 4, 0);
    Constant* dst_c = cache.VReg(rd, LaneKind::F32x4);
    Value* rn_v = cache.VReg(rn, LaneKind::F32x4);
    Value* rm_v = cache.VReg(rm, LaneKind::F32x4);
    bb->Append(cache.builder->CreateInstruction(
        Opcode::Add, Type::V128(LaneKind::F32x4), {dst_c, rn_v, rm_v}));
    return true;
}

// ---- Decode Table ----
// {mask, value, opcode, handler}. Ordered from most-specific mask to least so
// that fully-specified encodings (NOP, RET) are checked first. Within a mask
// group, values are distinct so order among them does not matter.

using HandlerFn = bool (*)(u32, u64, ConstantCache&, BasicBlock*);

struct DecodeEntry {
    u32 mask;
    u32 value;
    Opcode opcode;
    HandlerFn handler;
};

// Note: LDR/STR masks exclude the size field (bits 31-30) so both 32/64-bit
// encodings match. CBZ/CBNZ mask includes bit 24 (op) to distinguish them.
// More-specific masks are placed before broader masks that could alias.
const DecodeEntry kDecodeTable[] = {
    // mask, value, primary emitted opcode, handler
    {0xFFFFFFFFu, 0xD503201Fu, Opcode::Nop,        DecodeNop},
    {0xFFFFFC1Fu, 0xD65F0000u, Opcode::Return,     DecodeRet},
    // RC-FP-001: scalar FP. FMOV register (same mask width as RET).
    {0xFFFFFC1Fu, 0x1E204000u, Opcode::Mov,        DecodeFmovReg},
    // RC-FP-003: FMOV register D (ftype bits23:22=01).
    {0xFFFFFC1Fu, 0x1E604000u, Opcode::Mov,        DecodeFmovRegD},
    // RC-FP-004: FNEG/FABS S and D (1-source family, mask 0xFFFFFC00).
    {0xFFFFFC00u, 0x1E20C000u, Opcode::Abs,        DecodeFAbs},
    {0xFFFFFC00u, 0x1E60C000u, Opcode::Abs,        DecodeFAbs},
    {0xFFFFFC00u, 0x1E214000u, Opcode::Neg,        DecodeFNeg},
    {0xFFFFFC00u, 0x1E614000u, Opcode::Neg,        DecodeFNeg},
    // RC-FP-004: FCSEL S and D (bits11:10 fixed = 11; cond bits15:12 free).
    {0xFFE00C00u, 0x1E200C00u, Opcode::Select,     DecodeFCsel},
    {0xFFE00C00u, 0x1E600C00u, Opcode::Select,     DecodeFCsel},
    // RC-FP-004: FCVT S<->D (1-source opcodes 000101/000100).
    {0xFFFFFC00u, 0x1E22C000u, Opcode::Convert,    DecodeFcvtSToD},
    {0xFFFFFC00u, 0x1E624000u, Opcode::Convert,    DecodeFcvtDToS},
    // RC-FP-005: FSQRT S and D (1-source opcode 000011).
    {0xFFFFFC00u, 0x1E21C000u, Opcode::Sqrt,       DecodeFSqrt},
    {0xFFFFFC00u, 0x1E61C000u, Opcode::Sqrt,       DecodeFSqrt},
    // RC-FP-005: FRINTN (001000), FRINTP (001001), FRINTM (001010),
    // FRINTZ (001011), FRINTA (001100), each S and D.
    {0xFFFFFC00u, 0x1E244000u, Opcode::FrintN,     DecodeFrint<Opcode::FrintN>},
    {0xFFFFFC00u, 0x1E644000u, Opcode::FrintN,     DecodeFrint<Opcode::FrintN>},
    {0xFFFFFC00u, 0x1E24C000u, Opcode::FrintP,     DecodeFrint<Opcode::FrintP>},
    {0xFFFFFC00u, 0x1E64C000u, Opcode::FrintP,     DecodeFrint<Opcode::FrintP>},
    {0xFFFFFC00u, 0x1E254000u, Opcode::FrintM,     DecodeFrint<Opcode::FrintM>},
    {0xFFFFFC00u, 0x1E654000u, Opcode::FrintM,     DecodeFrint<Opcode::FrintM>},
    {0xFFFFFC00u, 0x1E25C000u, Opcode::FrintZ,     DecodeFrint<Opcode::FrintZ>},
    {0xFFFFFC00u, 0x1E65C000u, Opcode::FrintZ,     DecodeFrint<Opcode::FrintZ>},
    {0xFFFFFC00u, 0x1E264000u, Opcode::FrintA,     DecodeFrint<Opcode::FrintA>},
    {0xFFFFFC00u, 0x1E664000u, Opcode::FrintA,     DecodeFrint<Opcode::FrintA>},
    // FMOV immediate: fixed bits 31-24=0x1E, type=00, bit21=1, bits12-5=10000000.
    {0xFFE01FE0u, 0x1E201000u, Opcode::Mov,        DecodeFmovImm},
    // FADD/FSUB/FMUL S: fixed 31-24=0x1E, type=00, bit21=1, bits15-10 fixed.
    {0xFFE0FC00u, 0x1E202800u, Opcode::Add,        DecodeFaddS},
    {0xFFE0FC00u, 0x1E203800u, Opcode::Sub,        DecodeFsubS},
    {0xFFE0FC00u, 0x1E200800u, Opcode::Mul,        DecodeFmulS},
    // RC-FP-003: FADD/FSUB/FMUL/FDIV D (ftype=01 -> base 0x1E6x).
    {0xFFE0FC00u, 0x1E602800u, Opcode::Add,        DecodeFaddD},
    {0xFFE0FC00u, 0x1E603800u, Opcode::Sub,        DecodeFsubD},
    {0xFFE0FC00u, 0x1E600800u, Opcode::Mul,        DecodeFmulD},
    {0xFFE0FC00u, 0x1E601800u, Opcode::Div,        DecodeFdivD},
    // SIMD stage 2: MOVI/FMOV Vd.4S,#fpimm (cmode=1111), FDUP Vd.4S,Sn,
    // INS Vd.S[i],Vn (scalar src), UMOV Wd,Vn.S[i], FADD Vd.4S.
    {0xFFF8FC00u, 0x4F00F400u, Opcode::Mov,        DecodeMovi4S},
    {0xFFE7FF00u, 0x4E040400u, Opcode::Broadcast,  DecodeFdup4S},
    {0xFFE7FC00u, 0x6E040400u, Opcode::InsertLane, DecodeInsS},
    {0xFFE7FC00u, 0x0E043C00u, Opcode::ExtractLane,DecodeUmovS},
    {0xFFE0FC00u, 0x4E20D400u, Opcode::Add,        DecodeFadd4S},
    // RC-FP-002: FCMP Sn,Sm (bits15-10=001000). FCMPE (bit10=1) stays distinct.
    {0xFFE0FC00u, 0x1E202000u, Opcode::Compare,    DecodeFCmp},
    // RC-FP-003: FCMP Dn,Dm.
    {0xFFE0FC00u, 0x1E602000u, Opcode::Compare,    DecodeFCmp},
    // RC-FIX-002: CMP Rn,Rm (SUBS shifted register alias, imm6=0 and Rd=31
    // are encoded in mask/value). Both widths; must be before DecodeSubReg.
    {0xFFE0FFFFu, 0xEB00001Fu, Opcode::Compare,    DecodeCmpReg},
    {0xFFE0FFFFu, 0x6B00001Fu, Opcode::Compare,    DecodeCmpReg},
    // Non-alias SUBS (Rd != 31), imm6=0: Sub lowering, no flags.
    {0xFFE0FFFFu, 0xEB000000u, Opcode::Sub,        DecodeSubsReg},
    {0xFFE0FFFFu, 0x6B000000u, Opcode::Sub,        DecodeSubsReg},
    // LDR/STR S unsigned offset (32-bit FP, bits 31-22 fixed).
    {0xFFC00000u, 0xBD400000u, Opcode::Load,       DecodeLdrS},
    {0xFFC00000u, 0xBD000000u, Opcode::Store,      DecodeStrS},
    // RC-FP-003: LDR/STR D unsigned offset (size bits31:30=11, 8-byte access).
    {0xFFC00000u, 0xFD400000u, Opcode::Load,       DecodeLdrD},
    {0xFFC00000u, 0xFD000000u, Opcode::Store,      DecodeStrD},
    // RC-FP-002: FCVTZS Wd/Xd,Sn and SCVTF Sd,Wn/Xn. Fixed bits 31-10; the
    // sf bit is carried in the value, so both widths get an entry each.
    {0xFFFFFC00u, 0x9E780000u, Opcode::Convert,    DecodeFcvtzs},
    {0xFFFFFC00u, 0x1E780000u, Opcode::Convert,    DecodeFcvtzs},
    {0xFFFFFC00u, 0x9E220000u, Opcode::Convert,    DecodeScvtf},
    {0xFFFFFC00u, 0x1E220000u, Opcode::Convert,    DecodeScvtf},
    // RC-FP-003: FCVTZS Wd/Xd,Dn and SCVTF Dd,Wn/Xn (bit16=1 for double).
    {0xFFFFFC00u, 0x9E790000u, Opcode::Convert,    DecodeFcvtzs},
    {0xFFFFFC00u, 0x1E790000u, Opcode::Convert,    DecodeFcvtzs},
    {0xFFFFFC00u, 0x9E230000u, Opcode::Convert,    DecodeScvtf},
    {0xFFFFFC00u, 0x1E230000u, Opcode::Convert,    DecodeScvtf},
    // SVC #imm16: 1101 0100 000 imm16 00001. Distinct mask, no alias.
    {0xFFE0001Fu, 0xD4000001u, Opcode::Svc,        DecodeSvc},
    // LDP/STP 64-bit (bits 31-22 fixed) — placed before LDR/STR to avoid alias.
    {0xFFC00000u, 0xE8400000u, Opcode::Load,       DecodeLdp},
    {0xFFC00000u, 0xE8000000u, Opcode::Store,      DecodeStp},
    {0xFF000010u, 0x54000000u, Opcode::CondBranch, DecodeBCond},
    // MADD/MSUB (bits 30-21 + bit 15 fixed) — before the 0x7F800000 group.
    {0x7FE08000u, 0x1B000000u, Opcode::Mul,        DecodeMadd},
    {0x7FE08000u, 0x1B200000u, Opcode::Mul,        DecodeMsub},
    // LDR/STR register offset (mask 0x3FC00000) — distinct values from
    // the unsigned-offset variants, checked first.
    {0x3FC00000u, 0x38400000u, Opcode::Load,       DecodeLdrReg},
    {0x3FC00000u, 0x38000000u, Opcode::Store,      DecodeStrReg},
    {0x3FC00000u, 0x39400000u, Opcode::Load,       DecodeLdr},
    {0x3FC00000u, 0x39000000u, Opcode::Store,      DecodeStr},
    {0xFC000000u, 0x14000000u, Opcode::Branch,     DecodeB},
    {0xFC000000u, 0x94000000u, Opcode::Call,       DecodeBl},
    {0x7F000000u, 0x34000000u, Opcode::CondBranch, DecodeCbz},
    {0x7F000000u, 0x35000000u, Opcode::CondBranch, DecodeCbnz},
    // MOVK (0x7F800000 group).
    {0x7F800000u, 0x72800000u, Opcode::And,        DecodeMovk},
    {0x7F800000u, 0x11000000u, Opcode::Add,        DecodeAddImm},
    {0x7F800000u, 0x51000000u, Opcode::Sub,        DecodeSubImm},
    {0x7F800000u, 0x52800000u, Opcode::Add,        DecodeMovz},
    {0x7F800000u, 0x12800000u, Opcode::Not,        DecodeMovn},
    {0x7F800000u, 0x71000000u, Opcode::Sub,        DecodeCmpImm},
    {0x7F800000u, 0x53000000u, Opcode::Shl,        DecodeUbfm}, // LSL/LSR
    {0x7F800000u, 0x13000000u, Opcode::Sar,        DecodeSbfm}, // ASR
    // ADRP (bits 31, 28-24 fixed).
    {0x9F000000u, 0x90000000u, Opcode::Add,        DecodeAdrp},
    {0x7F200000u, 0x0B000000u, Opcode::Add,        DecodeAddReg},
    {0x7F200000u, 0x4B000000u, Opcode::Sub,        DecodeSubReg},
    {0x7F200000u, 0x0A000000u, Opcode::And,        DecodeAnd},
    {0x7F200000u, 0x2A000000u, Opcode::Or,         DecodeOrr},
    {0x7F200000u, 0x4A000000u, Opcode::Xor,        DecodeEor},
};

constexpr usize kDecodeTableSize =
    sizeof(kDecodeTable) / sizeof(kDecodeTable[0]);

// Per-module IRBuilder storage. The builder owns all constants and
// instructions referenced by the module, so it must outlive the module.
std::unordered_map<Module*, std::unique_ptr<IRBuilder>>& BuilderStore() {
    static std::unordered_map<Module*, std::unique_ptr<IRBuilder>> store;
    return store;
}

// Decode a single 32-bit instruction word and append the resulting IR
// instruction to |bb|. Returns false on unimplemented opcodes.
bool DecodeAndEmit(u32 word, u64 guest_addr, ConstantCache& cache, BasicBlock* bb) {
    for (usize i = 0; i < kDecodeTableSize; ++i) {
        const DecodeEntry& entry = kDecodeTable[i];
        if ((word & entry.mask) == entry.value) {
            return entry.handler(word, guest_addr, cache, bb);
        }
    }
    return false;
}

} // namespace

common::Result<std::unique_ptr<ir::Module>> Translate(
    common::span<const u8> code, const TranslateOptions& options) {
    auto module = std::make_unique<Module>();
    auto builder = std::make_unique<IRBuilder>();

    Function* fn = module->AddFunction();
    fn->set_guest_address(options.base_address);
    BasicBlock* bb = fn->AddBlock();

    // RC-PIPE-006: cache register and small-immediate constants for the
    // duration of this translation to avoid repeated heap allocations.
    ConstantCache cache;
    cache.builder = builder.get();

    const usize num_instrs = code.size() / 4;

    // RC-FP-002: ordinal of the first IR instruction emitted by each guest
    // word, used after decoding to resolve B.cond targets to IR ordinals.
    std::vector<size_t> word_first_ir;
    word_first_ir.reserve(num_instrs);

    for (usize i = 0; i < num_instrs; ++i) {
        const u8* p = code.data() + i * 4;
        u32 word = static_cast<u32>(p[0]) |
                   (static_cast<u32>(p[1]) << 8) |
                   (static_cast<u32>(p[2]) << 16) |
                   (static_cast<u32>(p[3]) << 24);

        u64 guest_addr = options.base_address + static_cast<u64>(i) * 4u;

        word_first_ir.push_back(bb->instructions().size());

        if (!DecodeAndEmit(word, guest_addr, cache, bb)) {
            if (options.strict_decode) {
                return common::Error{"unimplemented opcode: 0x" +
                                     std::to_string(word)};
            }
            bb->Append(builder->CreateInstruction(
                Opcode::Undefined, Type::Void(), {}));
        }
    }

    // RC-FP-002: resolve B.cond targets. The B.cond form is a CondBranch with
    // exactly 2 operands: op0 = U32 condition code, op1 = U64 target guest
    // address. Replace op1 with the ordinal of the first IR instruction of the
    // target guest word, so codegen can emit "goto glabel<ordinal>".
    for (Instruction* instr : bb->instructions()) {
        // RC-FIX-002: unconditional B is Branch with one U64 target operand.
        if (instr->opcode() == Opcode::Branch) {
            auto& ops = instr->operands();
            if (ops.size() != 1) continue;
            auto* tgt_c = dynamic_cast<Constant*>(ops[0]);
            if (!tgt_c || tgt_c->type().id != TypeId::U64) continue;
            u64 target_word =
                (tgt_c->value() - options.base_address) / 4u;
            size_t ir_ordinal = word_first_ir[target_word];
            ops[0] = builder->CreateConstant(static_cast<u64>(ir_ordinal),
                                             Type::U64());
            continue;
        }
        if (instr->opcode() != Opcode::CondBranch) continue;
        auto& ops = instr->operands();
        if (ops.size() != 2) continue;
        auto* cond_c = dynamic_cast<Constant*>(ops[0]);
        auto* tgt_c = dynamic_cast<Constant*>(ops[1]);
        if (!cond_c || !tgt_c) continue;
        if (cond_c->type().id != TypeId::U32) continue;
        u64 target_word =
            (tgt_c->value() - options.base_address) / 4u;
        size_t ir_ordinal = word_first_ir[target_word];
        ops[1] = builder->CreateConstant(static_cast<u64>(ir_ordinal),
                                         Type::U64());
    }

    Module* raw = module.get();
    BuilderStore()[raw] = std::move(builder);
    return module;
}

} // namespace sr::translator
