// RC-PIPE-003: codegen C backend — lower IR Module to C11 source.
// RC-PIPE-006: hot-path optimizations — std::string+reserve output buffer,
// fast integer-to-decimal conversion, direct append (no intermediate strings,
// no std::to_string / std::ostringstream on the per-instruction path).
#include "switchrecomp/codegen/codegen.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace sr::codegen {

namespace {

using sr::common::u32;
using sr::common::u64;

// Fast unsigned 64-bit -> decimal. Writes into |buf| (must hold >= 21 chars)
// and returns the number of characters written. Avoids std::to_string's
// locale/locale allocation overhead.
int FastU64ToDec(u64 v, char* buf) {
    if (v == 0) { buf[0] = '0'; return 1; }
    char tmp[21];
    int n = 0;
    while (v > 0) {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    for (int i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    return n;
}

// Append a u64 in decimal to |out|.
void AppendDec(std::string& out, u64 v) {
    char buf[21];
    int n = FastU64ToDec(v, buf);
    out.append(buf, static_cast<size_t>(n));
}

// Append a u64 in lowercase hex to |out|.
void AppendHex(std::string& out, u64 v) {
    if (v == 0) { out.push_back('0'); return; }
    char tmp[17];
    int n = 0;
    while (v > 0) {
        int d = static_cast<int>(v & 0xF);
        tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + (d - 10));
        v >>= 4;
    }
    for (int i = 0; i < n; ++i) out.push_back(tmp[n - 1 - i]);
}

// Register constants are modeled as constants holding the register index:
//  - integer registers: U64 type, index 0..30 (31 = ZR, excluded)
//  - FP S registers:    F32 type, index 0..31
//  - FP D registers:    F64 type, index 0..31
// F32 constants with a value >= 32 are always FMOV-immediate bit patterns
// (the smallest encodable f32 constant is 0.125 = 0x3E000000), so the index
// range uniquely identifies register references. Any F64 bit-pattern
// constant is likewise far above the index range.
bool IsXRegConstant(const ir::Constant* c) {
    return c->type().id == ir::TypeId::U64 && c->value() < 31;
}
bool IsFRegConstant(const ir::Constant* c) {
    return c->type().id == ir::TypeId::F32 && c->value() < 32;
}
bool IsDRegConstant(const ir::Constant* c) {
    return c->type().id == ir::TypeId::F64 && c->value() < 32;
}
// SIMD stage 2: vector register index constant (V128 typed, value < 32).
bool IsVRegConstant(const ir::Constant* c) {
    return c->type().id == ir::TypeId::V128 && c->value() < 32;
}
bool IsRegConstant(const ir::Constant* c) {
    return IsXRegConstant(c) || IsFRegConstant(c) || IsDRegConstant(c) ||
           IsVRegConstant(c);
}

// Monotonic counter for generated block-local temporaries (names must be
// unique across the whole translation unit).
u64 NextTempId() {
    static u64 counter = 0;
    return ++counter;
}

u64 RegIndex(const ir::Value* v) {
    if (auto* c = dynamic_cast<const ir::Constant*>(v)) {
        if (IsRegConstant(c)) return c->value();
    }
    return 0;
}

// Append an exact-round-trip f32 literal, e.g. "(float)1.5", from raw bits.
void AppendF32BitsLiteral(u32 bits, std::string& out) {
    union { u32 u; float f; } m;
    m.u = bits;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(m.f));
    out.append("(float)");
    out.append(buf);
}

// Append an IR operand's C representation to |out|.
//  - Constant U64 with value < 31 -> register ctx->x[N]
//  - Constant F32 with value < 32 -> register sr_v_get_s(&ctx->v[N])
//  - Constant F64 with value < 32 -> register sr_v_get_d(&ctx->v[N])
//  - Constant F32 otherwise       -> float literal (%.9g, exact round-trip)
//  - Constant F64 otherwise       -> double literal (%.17g, exact round-trip)
//  - other constants              -> immediate decimal
//  - Instruction                  -> temp variable v<id>
void Stringify(const ir::Value* v, std::string& out) {
    if (!v) { out.push_back('0'); return; }
    if (auto* c = dynamic_cast<const ir::Constant*>(v)) {
        if (IsXRegConstant(c)) {
            out.append("ctx->x[");
            AppendDec(out, c->value());
            out.push_back(']');
            return;
        }
        if (IsFRegConstant(c)) {
            out.append("sr_v_get_s(&ctx->v[");
            AppendDec(out, c->value());
            out.append("])");
            return;
        }
        if (IsDRegConstant(c)) {
            out.append("sr_v_get_d(&ctx->v[");
            AppendDec(out, c->value());
            out.append("])");
            return;
        }
        if (c->type().id == ir::TypeId::F32) {
            union { u32 u; float f; } m;
            m.u = static_cast<u32>(c->value());
            char buf[32];
            // 9 significant digits guarantee exact f32 round-trip.
            std::snprintf(buf, sizeof(buf), "%.9g",
                          static_cast<double>(m.f));
            out.append("(float)");
            out.append(buf);
            return;
        }
        if (c->type().id == ir::TypeId::F64) {
            union { u64 u; double d; } m;
            m.u = c->value();
            char buf[64];
            // 17 significant digits guarantee exact double round-trip.
            std::snprintf(buf, sizeof(buf), "%.17g", m.d);
            out.append(buf);
            return;
        }
        AppendDec(out, c->value());
        return;
    }
    if (auto* i = dynamic_cast<const ir::Instruction*>(v)) {
        out.push_back('v');
        AppendDec(out, i->id());
        return;
    }
    out.push_back('0');
}

// RC-FP-002: signed integer register constants carry an S32/S64 type (ordinary
// integer register constants are U64). Used by Convert (SCVTF) to know the
// source width and emit the matching signed cast.
bool IsSignedIntRegConstant(const ir::Constant* c) {
    if (!c || c->value() >= 31) return false;
    const ir::TypeId id = c->type().id;
    return id == ir::TypeId::S32 || id == ir::TypeId::S64;
}

// RC-FP-002/003: stringify an operand in a floating-point compare context.
// |is_double| selects the F64 register bank and literal form.
//  - FP reg constant                  -> sr_v_get_s/d(&ctx->v[N])
//  - FP bit-pattern constant          -> exact round-trip literal
//  - other constant (e.g. U32 zero)   -> ((float)/(double)<decimal>
//  - instruction                      -> v<id>
void FpStringify(const ir::Value* v, std::string& out, bool is_double) {
    if (auto* c = dynamic_cast<const ir::Constant*>(v)) {
        if (!is_double && IsFRegConstant(c)) {
            out.append("sr_v_get_s(&ctx->v[");
            AppendDec(out, c->value());
            out.append("])");
            return;
        }
        if (is_double && IsDRegConstant(c)) {
            out.append("sr_v_get_d(&ctx->v[");
            AppendDec(out, c->value());
            out.append("])");
            return;
        }
        if (!is_double && c->type().id == ir::TypeId::F32) {
            union { u32 u; float f; } m;
            m.u = static_cast<u32>(c->value());
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.9g",
                          static_cast<double>(m.f));
            out.append("(float)");
            out.append(buf);
            return;
        }
        if (is_double && c->type().id == ir::TypeId::F64) {
            union { u64 u; double d; } m;
            m.u = c->value();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", m.d);
            out.append(buf);
            return;
        }
        out.append(is_double ? "((double)" : "((float)");
        AppendDec(out, c->value());
        out.push_back(')');
        return;
    }
    Stringify(v, out);
}

// RC-FP-002: stringify a signed integer source for SCVTF. Register constants
// typed S32/S64 render as the matching signed read of ctx->x[N].
void SignedIntStringify(const ir::Value* v, std::string& out) {
    if (auto* c = dynamic_cast<const ir::Constant*>(v)) {
        if (IsSignedIntRegConstant(c)) {
            out.append(c->type().id == ir::TypeId::S32
                           ? "(int32_t)ctx->x["
                           : "(int64_t)ctx->x[");
            AppendDec(out, c->value());
            out.append("]");
            return;
        }
        if (IsXRegConstant(c)) {
            out.append("ctx->x[");
            AppendDec(out, c->value());
            out.push_back(']');
            return;
        }
        AppendDec(out, c->value());
        return;
    }
    Stringify(v, out);
}

// RC-FIX-002: append a Stringify'd operand wrapped in a C cast, e.g.
// "(int64_t)(ctx->x[0])". Used by the integer Compare lowering to build the
// signed/unsigned reads of each NZCV predicate without repeating casts.
void AppendWrapped(const ir::Value* v, const char* cast, std::string& out) {
    out.append(cast);
    out.push_back('(');
    Stringify(v, out);
    out.append(")");
}

// RC-FIX-002: append an integer CMP NZCV assignment. Width is selected by
// |is32|. All predicates are computed directly from the raw bit patterns:
//   N = signed a < signed b
//   Z = a == b          (width-masked)
//   C = unsigned a >= unsigned b   (SUBS carry = NOT borrow)
//   V = signed overflow of a - b, via the unsigned wrap result R:
//       bit63/31 of ((a ^ b) & (a ^ R))
// The subtraction is done in unsigned arithmetic, so there is no signed
// integer-overflow UB.
void EmitIntegerCompareNZCV(const ir::Value* a, const ir::Value* b,
                            bool is32, std::string& out) {
    const char* s_cast = is32 ? "(int32_t)" : "(int64_t)";
    const char* u_cast = is32 ? "(uint32_t)" : "(uint64_t)";
    const int sign_bit = is32 ? 31 : 63;

    out.append("ctx->nzcv = (uint32_t)(");

    // N
    out.append("((uint32_t)(");
    AppendWrapped(a, s_cast, out);
    out.append(" < ");
    AppendWrapped(b, s_cast, out);
    out.append(") << 31) | ");

    // Z
    out.append("((uint32_t)(");
    AppendWrapped(a, u_cast, out);
    out.append(" == ");
    AppendWrapped(b, u_cast, out);
    out.append(") << 30) | ");

    // C
    out.append("((uint32_t)(");
    AppendWrapped(a, u_cast, out);
    out.append(" >= ");
    AppendWrapped(b, u_cast, out);
    out.append(") << 29) | ");

    // V = ((a ^ b) & (a ^ (a - b))) >> sign_bit
    out.append("((uint32_t)(((");
    AppendWrapped(a, u_cast, out);
    out.append(" ^ ");
    AppendWrapped(b, u_cast, out);
    out.append(") & (");
    AppendWrapped(a, u_cast, out);
    out.append(" ^ (");
    AppendWrapped(a, u_cast, out);
    out.append(" - ");
    AppendWrapped(b, u_cast, out);
    out.append("))) >> ");
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", sign_bit);
    out.append(buf);
    out.append(") << 28));");
}

// RC-FP-002: append the C boolean expression for an ARM B.cond condition code
// over ctx->nzcv (N=bit31, Z=bit30, C=bit29, V=bit28). Covers all 16 codes;
// LT/LE/GT/GE compare N against V to implement the unordered semantics.
void AppendCondExpr(u64 cond, std::string& out) {
    const char* n = "((ctx->nzcv >> 31) & 1u)";
    const char* z = "((ctx->nzcv >> 30) & 1u)";
    const char* c = "((ctx->nzcv >> 29) & 1u)";
    const char* v = "((ctx->nzcv >> 28) & 1u)";
    switch (cond) {
        case 0: out.append(z); return;                                   // EQ
        case 1: out.append("("); out.append(z);
                out.append(" == 0)"); return;                            // NE
        case 2: out.append(c); return;                                   // CS/HS
        case 3: out.append("("); out.append(c);
                out.append(" == 0)"); return;                            // CC/LO
        case 4: out.append(n); return;                                   // MI
        case 5: out.append("("); out.append(n);
                out.append(" == 0)"); return;                            // PL
        case 6: out.append(v); return;                                   // VS
        case 7: out.append("("); out.append(v);
                out.append(" == 0)"); return;                            // VC
        case 8: out.append("("); out.append(c); out.append(" && (");
                out.append(z); out.append(" == 0))"); return;            // HI
        case 9: out.append("("); out.append(c);
                out.append(" == 0 || "); out.append(z);
                out.push_back(')'); return;                              // LS
        case 10: out.append("("); out.append(n); out.append(" == ");
                 out.append(v); out.push_back(')'); return;              // GE
        case 11: out.append("("); out.append(n); out.append(" != ");
                 out.append(v); out.push_back(')'); return;              // LT
        case 12: out.append("(("); out.append(z);
                 out.append(" == 0) && ("); out.append(n);
                 out.append(" == "); out.append(v);
                 out.append("))"); return;                               // GT
        case 13: out.append("("); out.append(z); out.append(" || (");
                 out.append(n); out.append(" != "); out.append(v);
                 out.append("))"); return;                               // LE
        case 14: out.push_back('1'); return;                             // AL
        default: out.push_back('0'); return;                             // NV
    }
}

// Collect ids of all Instruction operands referenced in a function so we can
// declare their temp variables at the top of the generated function.
void CollectReferencedInstructions(const ir::Function* fn,
                                   std::vector<u64>& ids) {
    for (const ir::BasicBlock* bb : fn->blocks()) {
        for (const ir::Instruction* i : bb->instructions()) {
            for (const ir::Value* op : i->operands()) {
                if (auto* oi = dynamic_cast<const ir::Instruction*>(op)) {
                    ids.push_back(oi->id());
                }
            }
        }
    }
}

// Lower a single IR instruction and append the C line directly to |out|.
// (No intermediate std::string is built.)
void LowerInstruction(const ir::Instruction* i, std::string& out) {
    using ir::Opcode;
    const auto& ops = i->operands();
    const ir::Value* op0 = ops.size() > 0 ? ops[0] : nullptr;
    const ir::Value* op1 = ops.size() > 1 ? ops[1] : nullptr;

    u64 rd = RegIndex(op0);

    switch (i->opcode()) {
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::Div: {
            // F32/F64 instructions use the explicit {dst, src1, src2} form;
            // integer ops keep the {src0/dst, src1} form.
            const ir::TypeId tid = i->type().id;
            const bool isf32 = tid == ir::TypeId::F32;
            const bool isf64 = tid == ir::TypeId::F64;
            if (tid == ir::TypeId::V128) {
                // SIMD stage 2, route A: per-lane scalar loop (F32x4).
                ir::LaneInfo info = ir::GetLaneInfo(i->type().lane);
                const char* vsym;
                if (i->opcode() == Opcode::Add) vsym = " + ";
                else if (i->opcode() == Opcode::Sub) vsym = " - ";
                else if (i->opcode() == Opcode::Div) vsym = " / ";
                else vsym = " * ";
                u64 vd = RegIndex(ops[0]);
                u64 vn = RegIndex(ops[1]);
                u64 vm = RegIndex(ops[2]);
                out.append("#pragma omp simd\n    for (int sr_i = 0; sr_i < ");
                AppendDec(out, static_cast<u64>(info.lanes));
                out.append("; ++sr_i) sr_v_set_lane_s(&ctx->v[");
                AppendDec(out, vd);
                out.append("], sr_i, sr_v_get_lane_s(&ctx->v[");
                AppendDec(out, vn);
                out.append("], sr_i)");
                out.append(vsym);
                out.append("sr_v_get_lane_s(&ctx->v[");
                AppendDec(out, vm);
                out.append("], sr_i));");
                return;
            }
            const ir::Value* left;
            const ir::Value* right;
            if (isf32 || isf64) {
                left = ops.size() > 1 ? ops[1] : nullptr;
                right = ops.size() > 2 ? ops[2] : nullptr;
            } else {
                left = op0;
                right = op1;
            }
            const char* sym;
            if (i->opcode() == Opcode::Add) sym = " + ";
            else if (i->opcode() == Opcode::Sub) sym = " - ";
            else if (i->opcode() == Opcode::Div) sym = " / ";
            else sym = " * ";

            if (isf32 || isf64) {
                // Scalar S/D write: low result via the helper, upper bits zeroed.
                out.append(isf32 ? "sr_v_set_s(&ctx->v[" : "sr_v_set_d(&ctx->v[");
                AppendDec(out, rd);
                out.append("], ");
                Stringify(left, out);
                out.append(sym);
                Stringify(right, out);
                out.append(");");
                return;
            }
            out.append("ctx->x[");
            AppendDec(out, rd);
            out.append("] = ");
            Stringify(left, out);
            out.append(sym);
            Stringify(right, out);
            out.push_back(';');
            return;
        }
        case Opcode::Mov: {
            // Operands: {dst register constant, src}.
            const ir::TypeId dtid =
                op0 ? op0->type().id : ir::TypeId::Void;
            if (dtid == ir::TypeId::V128) {
                // SIMD stage 2: MOVI Vd.4S -> local 4-float array loaded via
                // sr_v_load16 (works as both C11 and C++).
                u64 lo = 0, hi = 0;
                if (auto* cc = dynamic_cast<const ir::Constant*>(op1)) {
                    lo = cc->value();
                    hi = cc->value_hi();
                }
                u64 t = NextTempId();
                out.append("{ float sr_t");
                AppendDec(out, t);
                out.append("[4] = {");
                u32 lane_bits[4] = {
                    static_cast<u32>(lo), static_cast<u32>(lo >> 32),
                    static_cast<u32>(hi), static_cast<u32>(hi >> 32)};
                for (int k = 0; k < 4; ++k) {
                    if (k) out.append(", ");
                    AppendF32BitsLiteral(lane_bits[k], out);
                }
                out.append("}; sr_v_load16(&ctx->v[");
                AppendDec(out, RegIndex(op0));
                out.append("], sr_t");
                AppendDec(out, t);
                out.append("); }");
                return;
            }
            const bool dff = dtid == ir::TypeId::F32;
            const bool dfd = dtid == ir::TypeId::F64;
            if (dff || dfd) {
                out.append(dff ? "sr_v_set_s(&ctx->v[" : "sr_v_set_d(&ctx->v[");
                AppendDec(out, RegIndex(op0));
                out.append("], ");
                Stringify(op1, out);
                out.append(");");
                return;
            }
            out.append("ctx->x[");
            AppendDec(out, RegIndex(op0));
            out.append("] = ");
            Stringify(op1, out);
            out.push_back(';');
            return;
        }
        case Opcode::Neg: {
            // RC-FP-004: FNEG S/D. Operands {dst FReg/DReg constant, src}.
            const bool to_d =
                op0 && op0->type().id == ir::TypeId::F64;
            out.append(to_d ? "sr_v_set_d(&ctx->v[" : "sr_v_set_s(&ctx->v[");
            AppendDec(out, RegIndex(op0));
            out.append("], -");
            Stringify(op1, out);
            out.append(");");
            return;
        }
        case Opcode::Abs: {
            // RC-FP-004: FABS S/D -> fabsf()/fabs().
            const bool to_d =
                op0 && op0->type().id == ir::TypeId::F64;
            out.append(to_d ? "sr_v_set_d(&ctx->v[" : "sr_v_set_s(&ctx->v[");
            AppendDec(out, RegIndex(op0));
            out.append(to_d ? "], fabs(" : "], fabsf(");
            Stringify(op1, out);
            out.append("));");
            return;
        }
        case Opcode::Sqrt:
        case Opcode::FrintN:
        case Opcode::FrintP:
        case Opcode::FrintM:
        case Opcode::FrintZ:
        case Opcode::FrintA: {
            // RC-FP-005: 1-source FP unary lowered to a host libm call.
            //   FSQRT  -> sqrtf/sqrt
            //   FRINTN -> rintf/rint (round ties-to-even; uses the host
            //             rounding mode, which defaults to ties-to-even)
            //   FRINTP -> ceilf/ceil,  FRINTM -> floorf/floor
            //   FRINTZ -> truncf/trunc, FRINTA -> roundf/round
            // See docs/design/fp_rounding.md for the exact mapping/limits.
            const bool to_d =
                op0 && op0->type().id == ir::TypeId::F64;
            const char* fn_s;
            const char* fn_d;
            switch (i->opcode()) {
                case Opcode::Sqrt:   fn_s = "sqrtf";  fn_d = "sqrt";  break;
                case Opcode::FrintN: fn_s = "rintf";  fn_d = "rint";  break;
                case Opcode::FrintP: fn_s = "ceilf";  fn_d = "ceil";  break;
                case Opcode::FrintM: fn_s = "floorf"; fn_d = "floor"; break;
                case Opcode::FrintZ: fn_s = "truncf"; fn_d = "trunc"; break;
                default:             fn_s = "roundf"; fn_d = "round"; break;
            }
            out.append(to_d ? "sr_v_set_d(&ctx->v[" : "sr_v_set_s(&ctx->v[");
            AppendDec(out, RegIndex(op0));
            out.append("], ");
            out.append(to_d ? fn_d : fn_s);
            out.push_back('(');
            Stringify(op1, out);
            out.append("));");
            return;
        }
        case Opcode::Select: {
            // RC-FP-004: FCSEL. Operands {dst, U32 cond, if_true, if_false}.
            const ir::Value* op2 = ops.size() > 2 ? ops[2] : nullptr;
            const ir::Value* op3 = ops.size() > 3 ? ops[3] : nullptr;
            const bool to_d =
                op0 && op0->type().id == ir::TypeId::F64;
            u64 cond = 0;
            if (auto* cc = dynamic_cast<const ir::Constant*>(op1))
                cond = cc->value();
            out.append(to_d ? "sr_v_set_d(&ctx->v[" : "sr_v_set_s(&ctx->v[");
            AppendDec(out, RegIndex(op0));
            out.append("], (");
            AppendCondExpr(cond, out);
            out.append(") ? (");
            Stringify(op2, out);
            out.append(") : (");
            Stringify(op3, out);
            out.append("));");
            return;
        }
        case Opcode::Broadcast: {
            // FDUP Vd.4S, Sn: copy scalar Sn to all four lanes.
            u64 vd = RegIndex(op0);
            out.append("for (int sr_i = 0; sr_i < 4; ++sr_i) "
                       "sr_v_set_lane_s(&ctx->v[");
            AppendDec(out, vd);
            out.append("], sr_i, ");
            Stringify(op1, out);
            out.append(");");
            return;
        }
        case Opcode::InsertLane: {
            // INS Vd.S[i], Sn: write one lane, all other bytes preserved.
            u64 vd = RegIndex(op0);
            u64 lane = 0;
            if (auto* lc = dynamic_cast<const ir::Constant*>(op1))
                lane = lc->value();
            out.append("sr_v_set_lane_s(&ctx->v[");
            AppendDec(out, vd);
            out.append("], ");
            AppendDec(out, lane);
            out.append(", ");
            Stringify(ops.size() > 2 ? ops[2] : nullptr, out);
            out.append(");");
            return;
        }
        case Opcode::ExtractLane: {
            // UMOV Wd, Vn.S[i]: read one lane, reinterpret f32 bits as u32
            // (union, same convention as the F32 literal rendering), and
            // zero-extend into the W register slot.
            u64 wd = RegIndex(op0);
            u64 lane = 0;
            if (auto* lc = dynamic_cast<const ir::Constant*>(ops[2]))
                lane = lc->value();
            u64 t = NextTempId();
            out.append("{ union { float f; uint32_t u; } sr_u");
            AppendDec(out, t);
            out.append("; sr_u");
            AppendDec(out, t);
            out.append(".f = sr_v_get_lane_s(&ctx->v[");
            AppendDec(out, RegIndex(op1));
            out.append("], ");
            AppendDec(out, lane);
            out.append("); ctx->x[");
            AppendDec(out, wd);
            out.append("] = sr_u");
            AppendDec(out, t);
            out.append(".u; }");
            return;
        }
        case Opcode::And:
            out.append("ctx->x["); AppendDec(out, rd); out.append("] = ");
            Stringify(op0, out); out.append(" & "); Stringify(op1, out); out.push_back(';');
            return;
        case Opcode::Or:
            out.append("ctx->x["); AppendDec(out, rd); out.append("] = ");
            Stringify(op0, out); out.append(" | "); Stringify(op1, out); out.push_back(';');
            return;
        case Opcode::Xor:
            out.append("ctx->x["); AppendDec(out, rd); out.append("] = ");
            Stringify(op0, out); out.append(" ^ "); Stringify(op1, out); out.push_back(';');
            return;
        case Opcode::Not:
            out.append("ctx->x["); AppendDec(out, rd); out.append("] = ~");
            Stringify(op0, out); out.push_back(';');
            return;
        case Opcode::Shl:
            out.append("ctx->x["); AppendDec(out, rd); out.append("] = ");
            Stringify(op0, out); out.append(" << "); Stringify(op1, out); out.push_back(';');
            return;
        case Opcode::Shr:
            out.append("ctx->x["); AppendDec(out, rd); out.append("] = (uint64_t)");
            Stringify(op0, out); out.append(" >> "); Stringify(op1, out); out.push_back(';');
            return;
        case Opcode::Sar:
            out.append("ctx->x["); AppendDec(out, rd); out.append("] = (int64_t)");
            Stringify(op0, out); out.append(" >> "); Stringify(op1, out); out.push_back(';');
            return;
        case Opcode::Load: {
            const ir::TypeId ltid = i->type().id;
            const bool lf32 = ltid == ir::TypeId::F32;
            const bool lf64 = ltid == ir::TypeId::F64;
            u64 rt = RegIndex(op0);
            if (lf32) {
                out.append("sr_v_set_s(&ctx->v["); AppendDec(out, rt);
                out.append("], *(float*)sr_rt_guest_to_host(ctx, ");
                Stringify(op0, out); out.append(", 4, 0));");
                return;
            }
            if (lf64) {
                out.append("sr_v_set_d(&ctx->v["); AppendDec(out, rt);
                out.append("], *(double*)sr_rt_guest_to_host(ctx, ");
                Stringify(op0, out); out.append(", 8, 0));");
                return;
            }
            out.append("ctx->x["); AppendDec(out, rt);
            out.append("] = *(uint64_t*)sr_rt_guest_to_host(ctx, ");
            Stringify(op0, out); out.append(", 8, 0);");
            return;
        }
        case Opcode::Store: {
            const ir::TypeId stid =
                op1 ? op1->type().id : ir::TypeId::Void;
            const bool vf32 = stid == ir::TypeId::F32;
            const bool vf64 = stid == ir::TypeId::F64;
            u64 rt = RegIndex(op1);
            if (vf32) {
                out.append("*(float*)sr_rt_guest_to_host(ctx, ");
                Stringify(op0, out); out.append(", 4, 1) = sr_v_get_s(&ctx->v[");
                AppendDec(out, rt); out.append("]);");
                return;
            }
            if (vf64) {
                out.append("*(double*)sr_rt_guest_to_host(ctx, ");
                Stringify(op0, out); out.append(", 8, 1) = sr_v_get_d(&ctx->v[");
                AppendDec(out, rt); out.append("]);");
                return;
            }
            out.append("*(uint64_t*)sr_rt_guest_to_host(ctx, ");
            Stringify(op0, out); out.append(", 8, 1) = ctx->x[");
            AppendDec(out, rt); out.append("];");
            return;
        }
        case Opcode::Branch: {
            // RC-FIX-002: a Branch with one operand carries a resolved IR
            // instruction ordinal -> goto glabelN. Hand-built BasicBlock
            // branches (no operands) keep the legacy goto bb_ placeholder.
            u64 target = 0;
            if (auto* c = dynamic_cast<const ir::Constant*>(op0)) {
                target = c->value();
            }
            if (ops.size() == 1) {
                out.append("goto glabel");
            } else {
                out.append("goto bb_");
            }
            AppendDec(out, target);
            out.push_back(';');
            return;
        }
        case Opcode::CondBranch: {
            // RC-FP-002: B.cond form has 2 operands — op0 = U32 condition
            // code, op1 = U64 target IR ordinal. Emit a NZCV test with a
            // forward goto; false falls through, matching ARM semantics.
            const auto* cc = dynamic_cast<const ir::Constant*>(op0);
            if (ops.size() == 2 && cc &&
                cc->type().id == ir::TypeId::U32) {
                u64 target = 0;
                if (auto* tc = dynamic_cast<const ir::Constant*>(op1))
                    target = tc->value();
                out.append("if (");
                AppendCondExpr(cc->value(), out);
                out.append(") goto glabel");
                AppendDec(out, target);
                out.append(";");
                return;
            }
            // Other forms (CBZ/CBNZ, hand-built IR): existing placeholder.
            u64 cond = RegIndex(op0);
            out.append("if (ctx->x["); AppendDec(out, cond);
            out.append("]) goto bb_1; else goto bb_0;");
            return;
        }
        case Opcode::Return:
            out.append("return;");
            return;
        case Opcode::Call:
            out.append("sr_rt_call(ctx, ");
            Stringify(op0, out); out.append(");");
            return;
        case Opcode::IndirectBranch: {
            u64 reg = RegIndex(op0);
            out.append("sr_rt_call(ctx, ctx->x["); AppendDec(out, reg); out.append("]);");
            return;
        }
        case Opcode::Cast:
            out.append("ctx->x["); AppendDec(out, rd); out.append("] = (uint64_t)");
            Stringify(op0, out); out.push_back(';');
            return;
        case Opcode::Compare: {
            // RC-FIX-002/003: dispatch on the operand type. Integer CMP (both
            // operands integer-typed; the instruction type carries U32/U64
            // width) and FCMP S/D write the same ctx->nzcv.
            const ir::TypeId ctid =
                op0 ? op0->type().id : ir::TypeId::Void;
            const bool is_fp32 = ctid == ir::TypeId::F32;
            const bool is_fp64 = ctid == ir::TypeId::F64;
            if (!is_fp32 && !is_fp64) {
                const bool is32 = i->type().id == ir::TypeId::U32;
                EmitIntegerCompareNZCV(op0, op1, is32, out);
                return;
            }
            // RC-FP-002/003: FCMP S/D. Build NZCV directly from ordered C
            // compares: N = a<b, Z = a==b, C = a>=b (false on NaN),
            // V = unordered.
            const bool isd = is_fp64;
            out.append(
                "ctx->nzcv = (uint32_t)("
                "((uint32_t)(");
            FpStringify(op0, out, isd);
            out.append(" < ");
            FpStringify(op1, out, isd);
            out.append(") << 31) | ((uint32_t)(");
            FpStringify(op0, out, isd);
            out.append(" == ");
            FpStringify(op1, out, isd);
            out.append(") << 30) | ((uint32_t)(");
            FpStringify(op0, out, isd);
            out.append(" >= ");
            FpStringify(op1, out, isd);
            out.append(") << 29) | ((uint32_t)(((");
            FpStringify(op0, out, isd);
            out.append(" != ");
            FpStringify(op0, out, isd);
            out.append(") || (");
            FpStringify(op1, out, isd);
            out.append(" != ");
            FpStringify(op1, out, isd);
            out.append("))) << 28));");
            return;
        }
        case Opcode::Convert: {
            // RC-FP-002/003: operands {dst register constant, src}. Instruction
            // type is the destination type: S32/S64 for FCVTZS, F32/F64 for
            // SCVTF.
            const ir::TypeId dtid = i->type().id;
            u64 dst_idx = 0;
            if (auto* dc = dynamic_cast<const ir::Constant*>(op0))
                dst_idx = dc->value();

            if (dtid == ir::TypeId::F32 || dtid == ir::TypeId::F64) {
                // RC-FP-004: FCVT S<->D (source is also F32/F64).
                const ir::TypeId src_tid =
                    op1 ? op1->type().id : ir::TypeId::Void;
                if (src_tid == ir::TypeId::F32 ||
                    src_tid == ir::TypeId::F64) {
                    const bool to_d = dtid == ir::TypeId::F64;
                    out.append(to_d ? "sr_v_set_d(&ctx->v[" : "sr_v_set_s(&ctx->v[");
                    AppendDec(out, dst_idx);
                    out.append(to_d ? "], (double)" : "], (float)");
                    Stringify(op1, out);
                    out.append(");");
                    return;
                }
                // SCVTF Sd/Dd, Wn/Xn: matching float kind and signed cast.
                const bool to_d = dtid == ir::TypeId::F64;
                out.append(to_d ? "sr_v_set_d(&ctx->v[" : "sr_v_set_s(&ctx->v[");
                AppendDec(out, dst_idx);
                out.append(to_d ? "], (double)" : "], (float)");
                SignedIntStringify(op1, out);
                out.append(");");
                return;
            }

            // FCVTZS Wd/Xd, Sn/Dn with ARM saturating semantics. A bare C
            // float->int cast is undefined behavior when the value is out of
            // range (and on NaN), so emit an explicit, ordered clamp:
            // NaN -> 0; >= +2^N -> INT_MAX; <= -2^N -> INT_MIN; otherwise the
            // cast (guaranteed in range) truncates toward zero. The whole
            // expression is integer-typed and widened to uint64 for ctx->x[]
            // storage (well-defined).
            const bool src_d =
                op1 && op1->type().id == ir::TypeId::F64;
            const bool is64 = dtid == ir::TypeId::S64;
            out.append("ctx->x["); AppendDec(out, dst_idx);
            out.append("] = (uint64_t)(");
            if (is64) {
                // 2^63 is exactly representable as double (and f32). For a
                // double source the C11 hex float literal 0x1.0p63 is emitted
                // (exact by construction); the f32-source path keeps the
                // decimal double threshold (f32 is promoted for compares).
                const char* hi = src_d ? "0x1.0p63"
                                       : "9223372036854775808.0";
                const char* lo = src_d ? "-0x1.0p63"
                                       : "-9223372036854775808.0";
                out.append("isnan(");
                FpStringify(op1, out, src_d);
                out.append(") ? (int64_t)0 : (");
                FpStringify(op1, out, src_d);
                out.append(" >= ");
                out.append(hi);
                out.append(" ? (int64_t)"
                           "9223372036854775807LL : (");
                FpStringify(op1, out, src_d);
                out.append(" <= ");
                out.append(lo);
                out.append(" ? "
                           "(-9223372036854775807LL - 1) : "
                           "(int64_t)");
                FpStringify(op1, out, src_d);
                out.append("))");
            } else {
                // 2^31 = 2147483648.0 is exactly representable; any source
                // value at or beyond it saturates. INT_MIN is written as
                // (-2147483647 - 1) so no out-of-range integer literal or
                // implementation-defined narrowing is ever produced.
                const char* lit = src_d ? "2147483648.0" : "2147483648.0f";
                out.append("isnan(");
                FpStringify(op1, out, src_d);
                out.append(") ? 0 : (");
                FpStringify(op1, out, src_d);
                out.append(" >= ");
                out.append(lit);
                out.append(" ? 2147483647 : (");
                FpStringify(op1, out, src_d);
                out.append(" <= -");
                out.append(lit);
                out.append(" ? (-2147483647 - 1) : "
                           "(int32_t)");
                FpStringify(op1, out, src_d);
                out.append("))");
            }
            out.append(");");
            return;
        }
        case Opcode::Nop:
            out.append("/* nop */");
            return;
        case Opcode::Svc:
            out.append("sr_rt_handle_svc(ctx, ");
            Stringify(op0, out); out.append(");");
            return;
        case Opcode::Undefined:
            out.append("/* unimplemented opcode */");
            return;
    }
    out.append("/* unimplemented opcode */");
}

} // namespace

common::Result<std::string> GenerateC(const ir::Module& module,
                                      const CodegenOptions& /*options*/) {
    // RC-PIPE-006: use a single std::string with reserve instead of
    // std::ostringstream. The header is ~700 bytes; per-function body grows
    // roughly with instruction count, so reserve a conservative amount.
    std::string out;
    out.reserve(2048);

    out.append("#include <stdint.h>\n");
    // RC-FIX-002: isnan() for the FCVTZS saturation clamp.
    out.append("#include <math.h>\n\n");
    out.append("#ifdef __cplusplus\n");
    out.append("#include \"switchrecomp/runtime/runtime.h\"\n");
    out.append("using sr::runtime::GuestContext;\n");
    out.append("#else\n");
    // Fallback pure-C path: pull in sr_vreg + the sr_v_* accessors so the same
    // emitted lines work as C11. Resolved via the -I include dir on the host
    // compile command (same route runtime.h is found on the C++ path).
    out.append("#include \"switchrecomp/runtime/vreg.h\"\n");
    out.append("typedef struct GuestContext {\n");
    out.append("    uint64_t x[31];\n");
    out.append("    uint64_t sp;\n");
    out.append("    uint64_t pc;\n");
    out.append("    uint32_t nzcv;\n");
    out.append("    sr_vreg v[32];\n");
    out.append("    uint32_t fpcr;\n");
    out.append("    uint32_t fpsr;\n");
    out.append("    uint64_t shared_counter;\n");
    out.append("    uint64_t thread_handle;\n");
    out.append("} GuestContext;\n");
    out.append("#endif\n\n");
    out.append("#ifdef __cplusplus\n");
    out.append("extern \"C\" {\n");
    out.append("#endif\n");
    out.append("void* sr_rt_guest_to_host(GuestContext* ctx, uint64_t addr, uint64_t size, int write);\n");
    out.append("void  sr_rt_call(GuestContext* ctx, uint64_t target);\n");
    out.append("void  sr_rt_handle_svc(GuestContext* ctx, uint32_t svc_id);\n");
    out.append("void  sr_rt_register_function(uint64_t guest_addr, void(*fn)(GuestContext*));\n");
    out.append("#ifdef __cplusplus\n");
    out.append("}\n");
    out.append("#endif\n\n");

    for (const ir::Function* fn : module.functions()) {
        out.append("void fn_");
        AppendHex(out, fn->guest_address());
        out.append("(GuestContext* ctx) {\n");

        // RC-PIPE-006: use a sorted vector + unique instead of unordered_set
        // to avoid hash overhead on the hot path.
        std::vector<u64> ref_ids;
        CollectReferencedInstructions(fn, ref_ids);
        std::sort(ref_ids.begin(), ref_ids.end());
        ref_ids.erase(std::unique(ref_ids.begin(), ref_ids.end()), ref_ids.end());
        for (u64 id : ref_ids) {
            out.append("    uint64_t v");
            AppendDec(out, id);
            out.append(" = 0;\n");
        }

        size_t bb_idx = 0;
        // RC-FP-002: function-global ordinal of each IR instruction. Every
        // instruction gets a "glabel<N>:" label so B.cond can branch to the
        // resolved target ordinal from any point in the function.
        size_t instr_ordinal = 0;
        for (const ir::BasicBlock* bb : fn->blocks()) {
            out.append("bb_");
            AppendDec(out, bb_idx);
            out.append(":\n");
            for (const ir::Instruction* i : bb->instructions()) {
                out.append("    glabel");
                AppendDec(out, instr_ordinal);
                out.append(":\n    ");
                LowerInstruction(i, out);
                out.push_back('\n');
                ++instr_ordinal;
            }
            ++bb_idx;
        }
        out.append("}\n\n");
    }

    return out;
}

} // namespace sr::codegen
