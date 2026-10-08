#pragma once

#include "switchrecomp/common/types.h"

#include <memory>
#include <string>
#include <vector>

namespace sr::ir {

using sr::common::u8;
using sr::common::u16;
using sr::common::u32;
using sr::common::u64;

enum class TypeId : u8 {
    Void,
    U8, U16, U32, U64,
    S8, S16, S32, S64,
    F32, F64,
    Ptr,
    // SIMD stage 2: 128-bit vector register (lane layout given by LaneKind).
    V128,
};

// Lane layout for V128 values. Stage 2 only needs four 32-bit float lanes.
enum class LaneKind : u8 {
    None,
    F32x4,
};

struct Type {
    TypeId id;
    // Only meaningful when id == TypeId::V128; None otherwise.
    LaneKind lane = LaneKind::None;

    static Type Void() { return {TypeId::Void}; }
    static Type U8() { return {TypeId::U8}; }
    static Type U16() { return {TypeId::U16}; }
    static Type U32() { return {TypeId::U32}; }
    static Type U64() { return {TypeId::U64}; }
    static Type S32() { return {TypeId::S32}; }
    static Type S64() { return {TypeId::S64}; }
    static Type F32() { return {TypeId::F32}; }
    static Type F64() { return {TypeId::F64}; }
    static Type Ptr() { return {TypeId::Ptr}; }
    static Type V128(LaneKind k) { return {TypeId::V128, k}; }

    // SIMD stage 2: full equality includes the lane layout for V128.
    bool operator==(const Type& other) const {
        return id == other.id && lane == other.lane;
    }
    bool operator!=(const Type& other) const { return !(*this == other); }
};

// Static description of a LaneKind layout.
struct LaneInfo {
    int lanes;          // number of lanes
    int esize;          // element size in bits
    TypeId element;     // scalar element type
};
LaneInfo GetLaneInfo(LaneKind kind);

enum class Opcode : u16 {
    Nop,
    // Arithmetic
    Add, Sub, And, Or, Xor, Not, Shl, Shr, Sar, Mul,
    // RC-FP-003: floating-point divide (FDIV S/D)
    Div,
    // Memory
    Load, Store,
    // Control flow
    Branch, CondBranch, Return, Call, IndirectBranch,
    // Conversion
    Cast,
    // Special
    Svc, Undefined,
    // Register / value copy (RC-FP-001: used for scalar FP FMOV)
    Mov,
    // RC-FP-002: FP compare (writes NZCV) and type conversion (FCVTZS/SCVTF).
    Compare, Convert,
    // RC-FP-004: FP negate (FNEG), absolute value (FABS), conditional
    // select (FCSEL).
    Neg, Abs, Select,
    // RC-FP-005: FP square root (FSQRT S/D) and the round-to-integer family
    // (FRINTN/FRINTP/FRINTM/FRINTZ/FRINTA S/D). All are 1-source FP ops with
    // operands {dst register constant, src}.
    Sqrt, FrintN, FrintP, FrintM, FrintZ, FrintA,
    // SIMD stage 2: FDUP (scalar broadcast to all lanes), INS (insert one
    // lane), UMOV (extract one lane).
    Broadcast, InsertLane, ExtractLane,
};

// Base class for all SSA values.
class Value {
public:
    virtual ~Value() = default;
    Type type() const { return type_; }
    void set_type(Type t) { type_ = t; }

    // Unique id for serialization (assigned by IRBuilder).
    u64 id() const { return id_; }
    void set_id(u64 id) { id_ = id; }

protected:
    Type type_{Type::Void()};
    u64 id_{0};
};

// A constant value (immediate).
class Constant : public Value {
public:
    explicit Constant(u64 value, Type type) : value_(value) {
        type_ = type;
    }
    // 128-bit constant (value_lo = lanes 0/1, value_hi = lanes 2/3).
    Constant(u64 value_lo, u64 value_hi, Type type)
        : value_(value_lo), value_hi_(value_hi) {
        type_ = type;
    }
    u64 value() const { return value_; }
    u64 value_hi() const { return value_hi_; }

private:
    u64 value_;
    u64 value_hi_ = 0;
};

class Instruction : public Value {
public:
    Opcode opcode() const { return opcode_; }
    void set_opcode(Opcode op) { opcode_ = op; }

    const std::vector<Value*>& operands() const { return operands_; }
    std::vector<Value*>& operands() { return operands_; }
    void set_operands(std::vector<Value*> ops) { operands_ = std::move(ops); }

protected:
    Opcode opcode_{Opcode::Nop};
    std::vector<Value*> operands_;
};

class BasicBlock {
public:
    BasicBlock() = default;

    const std::vector<Instruction*>& instructions() const { return instrs_; }
    std::vector<Instruction*>& instructions() { return instrs_; }
    void Append(Instruction* i) { instrs_.push_back(i); }

    // Ownership: take ownership of an instruction.
    Instruction* AddOwned(std::unique_ptr<Instruction> i) {
        Instruction* raw = i.get();
        owned_.push_back(std::move(i));
        instrs_.push_back(raw);
        return raw;
    }

private:
    std::vector<Instruction*> instrs_;
    std::vector<std::unique_ptr<Instruction>> owned_;
};

class Function {
public:
    Function() = default;

    u64 guest_address() const { return guest_addr_; }
    void set_guest_address(u64 a) { guest_addr_ = a; }

    const std::vector<BasicBlock*>& blocks() const { return blocks_; }
    std::vector<BasicBlock*>& blocks() { return blocks_; }

    BasicBlock* AddBlock() {
        auto bb = std::make_unique<BasicBlock>();
        BasicBlock* raw = bb.get();
        owned_blocks_.push_back(std::move(bb));
        blocks_.push_back(raw);
        return raw;
    }

private:
    u64 guest_addr_{0};
    std::vector<BasicBlock*> blocks_;
    std::vector<std::unique_ptr<BasicBlock>> owned_blocks_;
};

class Module {
public:
    Module() = default;

    std::vector<Function*>& functions() { return funcs_; }
    const std::vector<Function*>& functions() const { return funcs_; }

    Function* AddFunction() {
        auto f = std::make_unique<Function>();
        Function* raw = f.get();
        owned_funcs_.push_back(std::move(f));
        funcs_.push_back(raw);
        return raw;
    }

    // Serialize to / from JSON (for debugging & golden tests).
    std::string to_json() const;
    static std::unique_ptr<Module> from_json(const std::string& json);

    // Structural verification (SSA, types). Returns true if valid.
    bool verify() const;

private:
    std::vector<Function*> funcs_;
    std::vector<std::unique_ptr<Function>> owned_funcs_;
};

// IRBuilder owns constants and provides a factory for instructions.
// The builder must outlive any Module that references its constants.
class IRBuilder {
public:
    IRBuilder() = default;

    Constant* CreateConstant(u64 value, Type type);
    // SIMD stage 2: 128-bit constant (lo 64 bits + hi 64 bits).
    Constant* CreateConstant(u64 value_lo, u64 value_hi, Type type);

    Instruction* CreateInstruction(Opcode op, Type type,
                                   std::vector<Value*> operands = {});

    // Convenience factories (common opcodes).
    Instruction* CreateAdd(Value* a, Value* b);
    Instruction* CreateSub(Value* a, Value* b);
    Instruction* CreateAnd(Value* a, Value* b);
    Instruction* CreateOr(Value* a, Value* b);
    Instruction* CreateXor(Value* a, Value* b);
    Instruction* CreateNot(Value* a);
    Instruction* CreateShl(Value* a, Value* b);
    Instruction* CreateShr(Value* a, Value* b);
    Instruction* CreateSar(Value* a, Value* b);
    Instruction* CreateMul(Value* a, Value* b);
    Instruction* CreateLoad(Value* addr, Type result_type);
    Instruction* CreateStore(Value* addr, Value* value);
    Instruction* CreateBranch(BasicBlock* target);
    Instruction* CreateCondBranch(Value* cond, BasicBlock* if_true, BasicBlock* if_false);
    Instruction* CreateReturn();
    Instruction* CreateCall(u64 target);
    Instruction* CreateIndirectBranch(Value* target);
    Instruction* CreateCast(Value* value, Type to_type);
    Instruction* CreateSvc(u32 svc_id);
    Instruction* CreateMov(Value* dst, Value* src);
    // RC-FP-004: scalar FP negate/absolute value ({dst, src}) and conditional
    // select ({dst, U32 condition code, if_true, if_false}).
    Instruction* CreateNeg(Value* dst, Value* src);
    Instruction* CreateAbs(Value* dst, Value* src);
    Instruction* CreateSelect(Value* dst, Value* cond, Value* if_true,
                              Value* if_false);
    // RC-FP-005: scalar FP square root and round-to-integer ({dst, src}).
    Instruction* CreateSqrt(Value* dst, Value* src);
    Instruction* CreateFrintN(Value* dst, Value* src);
    Instruction* CreateFrintP(Value* dst, Value* src);
    Instruction* CreateFrintM(Value* dst, Value* src);
    Instruction* CreateFrintZ(Value* dst, Value* src);
    Instruction* CreateFrintA(Value* dst, Value* src);
    // SIMD stage 2: FDUP {dst V128, src scalar F32}; INS {dst V128, U32 lane,
    // src scalar F32}; UMOV {dst U64 W register, src V128, U32 lane}.
    Instruction* CreateBroadcast(Value* dst, Value* src);
    Instruction* CreateInsertLane(Value* dst, Value* lane, Value* src);
    Instruction* CreateExtractLane(Value* dst, Value* src, Value* lane);

private:
    u64 next_id_{1};
    std::vector<std::unique_ptr<Value>> owned_; // owns constants
};

} // namespace sr::ir
