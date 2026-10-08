// T-RC-1-02: IR JSON serialization — round-trip tests
#include <gtest/gtest.h>

#include <cstring>

#include "switchrecomp/ir/ir.h"

using namespace sr::ir;

namespace {

// Holds a built module together with its IRBuilder. The builder owns the
// constants and instructions referenced by the module, so it must outlive
// any use of the module (see IRBuilder comment in ir.h).
struct BuiltModule {
    std::unique_ptr<IRBuilder> builder;
    std::unique_ptr<Module> module;
};

// Build a small module: fn with two blocks, arithmetic + branch,
// instruction-to-instruction operand reference.
BuiltModule BuildTestModule() {
    BuiltModule bm;
    bm.builder = std::make_unique<IRBuilder>();
    bm.module = std::make_unique<Module>();
    IRBuilder& builder = *bm.builder;
    Module& module = *bm.module;

    Function* fn = module.AddFunction();
    fn->set_guest_address(0x400000);

    BasicBlock* entry = fn->AddBlock();
    BasicBlock* exit_bb = fn->AddBlock();

    Constant* c1 = builder.CreateConstant(1, Type::U64());
    Constant* c2 = builder.CreateConstant(2, Type::U64());

    Instruction* add = builder.CreateAdd(c1, c2);
    entry->Append(add);

    // sub uses the result of add (instruction->instruction reference)
    Instruction* sub = builder.CreateSub(add, c1);
    entry->Append(sub);

    entry->Append(builder.CreateBranch(exit_bb));
    exit_bb->Append(builder.CreateReturn());

    return bm;
}

} // namespace

TEST(IrSerialize, EmptyModuleRoundTrip) {
    Module m;
    std::string json1 = m.to_json();
    auto m2 = Module::from_json(json1);
    std::string json2 = m2->to_json();
    EXPECT_EQ(json1, json2);
    EXPECT_EQ(m2->functions().size(), 0u);
}

TEST(IrSerialize, RoundTripPreservesStructure) {
    auto bm = BuildTestModule();
    Module* m = bm.module.get();
    std::string json1 = m->to_json();

    auto m2 = Module::from_json(json1);
    std::string json2 = m2->to_json();

    // The key invariant: to_json -> from_json -> to_json is stable.
    EXPECT_EQ(json1, json2);

    // Structure checks.
    ASSERT_EQ(m2->functions().size(), 1u);
    Function* fn = m2->functions()[0];
    EXPECT_EQ(fn->guest_address(), 0x400000u);
    ASSERT_EQ(fn->blocks().size(), 2u);

    BasicBlock* entry = fn->blocks()[0];
    ASSERT_EQ(entry->instructions().size(), 3u); // add, sub, branch

    Instruction* add = entry->instructions()[0];
    EXPECT_EQ(add->opcode(), Opcode::Add);
    EXPECT_EQ(add->type().id, TypeId::U64);
    ASSERT_EQ(add->operands().size(), 2u);
    // Both operands are constants.
    EXPECT_EQ(add->operands()[0]->id(), 1u);
    EXPECT_EQ(add->operands()[1]->id(), 2u);

    Instruction* sub = entry->instructions()[1];
    EXPECT_EQ(sub->opcode(), Opcode::Sub);
    ASSERT_EQ(sub->operands().size(), 2u);
    // First operand references the add instruction (id 3).
    EXPECT_EQ(sub->operands()[0]->id(), add->id());
    // Second operand is constant 1.
    EXPECT_EQ(sub->operands()[1]->id(), 1u);

    BasicBlock* exit_bb = fn->blocks()[1];
    ASSERT_EQ(exit_bb->instructions().size(), 1u);
    EXPECT_EQ(exit_bb->instructions()[0]->opcode(), Opcode::Return);
}

TEST(IrSerialize, ConstantValuesPreserved) {
    IRBuilder builder;
    Module m;
    Function* fn = m.AddFunction();
    BasicBlock* bb = fn->AddBlock();

    Constant* c = builder.CreateConstant(0xDEADBEEFCAFEBABEull, Type::U64());
    bb->Append(builder.CreateAdd(c, c));

    std::string json = m.to_json();
    auto m2 = Module::from_json(json);

    Function* fn2 = m2->functions()[0];
    Instruction* add = fn2->blocks()[0]->instructions()[0];
    ASSERT_EQ(add->operands().size(), 2u);
    auto* c0 = dynamic_cast<Constant*>(add->operands()[0]);
    ASSERT_NE(c0, nullptr);
    EXPECT_EQ(c0->value(), 0xDEADBEEFCAFEBABEull);
    EXPECT_EQ(c0->type().id, TypeId::U64);
}

TEST(IrSerialize, AllOpcodesRoundTrip) {
    IRBuilder builder;
    Module m;
    Function* fn = m.AddFunction();
    BasicBlock* bb = fn->AddBlock();

    Constant* c = builder.CreateConstant(0, Type::U64());

    bb->Append(builder.CreateAdd(c, c));
    bb->Append(builder.CreateSub(c, c));
    bb->Append(builder.CreateAnd(c, c));
    bb->Append(builder.CreateOr(c, c));
    bb->Append(builder.CreateXor(c, c));
    bb->Append(builder.CreateNot(c));
    bb->Append(builder.CreateShl(c, c));
    bb->Append(builder.CreateShr(c, c));
    bb->Append(builder.CreateSar(c, c));
    bb->Append(builder.CreateMul(c, c));
    bb->Append(builder.CreateLoad(c, Type::U64()));
    bb->Append(builder.CreateStore(c, c));
    bb->Append(builder.CreateBranch(nullptr));
    bb->Append(builder.CreateCondBranch(c, nullptr, nullptr));
    bb->Append(builder.CreateReturn());
    bb->Append(builder.CreateCall(0x1000));
    bb->Append(builder.CreateIndirectBranch(c));
    bb->Append(builder.CreateCast(c, Type::U32()));
    bb->Append(builder.CreateSvc(0x80));

    std::string json1 = m.to_json();
    auto m2 = Module::from_json(json1);
    std::string json2 = m2->to_json();
    EXPECT_EQ(json1, json2);

    // Verify all opcodes survived.
    Function* fn2 = m2->functions()[0];
    const auto& instrs = fn2->blocks()[0]->instructions();
    Opcode expected[] = {
        Opcode::Add, Opcode::Sub, Opcode::And, Opcode::Or, Opcode::Xor,
        Opcode::Not, Opcode::Shl, Opcode::Shr, Opcode::Sar, Opcode::Mul,
        Opcode::Load, Opcode::Store, Opcode::Branch, Opcode::CondBranch,
        Opcode::Return, Opcode::Call, Opcode::IndirectBranch, Opcode::Cast,
        Opcode::Svc,
    };
    ASSERT_EQ(instrs.size(), sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < instrs.size(); ++i) {
        EXPECT_EQ(instrs[i]->opcode(), expected[i]) << "opcode mismatch at " << i;
    }
}

TEST(IrSerialize, VerifyPassesAfterRoundTrip) {
    auto bm = BuildTestModule();
    std::string json = bm.module->to_json();
    auto m2 = Module::from_json(json);
    EXPECT_TRUE(m2->verify());
}

// RC-FP-001: an F32 constant stores the f32 bit pattern in the u64 value;
// the JSON round-trip must preserve the exact bits (verified as u32).
TEST(IrSerialize, F32ConstantBitPatternRoundTrip) {
    IRBuilder builder;
    Module m;
    Function* fn = m.AddFunction();
    BasicBlock* bb = fn->AddBlock();

    const float fv = 2.5f;
    u32 bits = 0;
    std::memcpy(&bits, &fv, sizeof(bits));
    ASSERT_EQ(bits, 0x40200000u);

    Constant* c = builder.CreateConstant(static_cast<u64>(bits), Type::F32());
    bb->Append(builder.CreateMov(
        builder.CreateConstant(0u, Type::F32()), c));

    std::string json = m.to_json();
    // The decimal u32 bit pattern must appear in the serialized form.
    EXPECT_NE(json.find("1075838976"), std::string::npos);
    EXPECT_NE(json.find("\"F32\""), std::string::npos);

    auto m2 = Module::from_json(json);
    // to_json -> from_json -> to_json is stable.
    EXPECT_EQ(m2->to_json(), json);

    Instruction* mov = m2->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(mov->opcode(), Opcode::Mov);
    EXPECT_EQ(mov->type().id, TypeId::F32);
    // Source is the F32 constant (operand index 1).
    auto* c2 = dynamic_cast<Constant*>(mov->operands()[1]);
    ASSERT_NE(c2, nullptr);
    EXPECT_EQ(c2->type().id, TypeId::F32);
    u32 out_bits = static_cast<u32>(c2->value());
    EXPECT_EQ(out_bits, bits);
    float out_f = 0.0f;
    std::memcpy(&out_f, &out_bits, sizeof(out_f));
    EXPECT_FLOAT_EQ(out_f, 2.5f);
}

// RC-FP-003: an F64 constant stores the IEEE-754 double bit pattern in the
// u64 value; the JSON round-trip must preserve the exact bits.
TEST(IrSerialize, F64ConstantBitPatternRoundTrip) {
    IRBuilder builder;
    Module m;
    Function* fn = m.AddFunction();
    BasicBlock* bb = fn->AddBlock();

    const double dv = 3.141592653589793;
    u64 bits = 0;
    std::memcpy(&bits, &dv, sizeof(bits));
    ASSERT_EQ(bits, 0x400921FB54442D18ull);

    Constant* c = builder.CreateConstant(bits, Type::F64());
    bb->Append(builder.CreateMov(
        builder.CreateConstant(0u, Type::F64()), c));

    std::string json = m.to_json();
    // The decimal u64 bit pattern must appear in the serialized form.
    EXPECT_NE(json.find("4614256656552045848"), std::string::npos);
    EXPECT_NE(json.find("\"F64\""), std::string::npos);

    auto m2 = Module::from_json(json);
    // to_json -> from_json -> to_json is stable.
    EXPECT_EQ(m2->to_json(), json);

    Instruction* mov = m2->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(mov->opcode(), Opcode::Mov);
    EXPECT_EQ(mov->type().id, TypeId::F64);
    auto* c2 = dynamic_cast<Constant*>(mov->operands()[1]);
    ASSERT_NE(c2, nullptr);
    EXPECT_EQ(c2->type().id, TypeId::F64);
    EXPECT_EQ(c2->value(), bits);
    double out_d = 0.0;
    std::memcpy(&out_d, &bits, sizeof(out_d));
    EXPECT_DOUBLE_EQ(out_d, dv);
}

// SIMD stage 2: a V128 (F32x4) register constant and a 128-bit MOVI payload
// constant must serialize with the "V128:F32x4" type string and the two
// hexadecimal payload halves, and survive the JSON round-trip unchanged.
TEST(IrSerialize, V128F32x4ConstantRoundTrip) {
    IRBuilder builder;
    Module m;
    Function* fn = m.AddFunction();
    BasicBlock* bb = fn->AddBlock();

    Constant* dst = builder.CreateConstant(0u, Type::V128(LaneKind::F32x4));
    // All four lanes = 1.5 (f32 bits 0x3FC00000).
    const u64 dup = 0x3fc00000ull | (0x3fc00000ull << 32);
    Constant* src = builder.CreateConstant(
        dup, dup, Type::V128(LaneKind::F32x4));
    bb->Append(builder.CreateMov(dst, src));

    std::string json = m.to_json();
    EXPECT_NE(json.find("\"V128:F32x4\""), std::string::npos);
    EXPECT_NE(json.find("\"value_lo\": \"0x3fc000003fc00000\""),
              std::string::npos);
    EXPECT_NE(json.find("\"value_hi\": \"0x3fc000003fc00000\""),
              std::string::npos);
    // No plain decimal "value" field is emitted for V128 constants.
    EXPECT_EQ(json.find("\"value\":"), std::string::npos);

    auto m2 = Module::from_json(json);
    // to_json -> from_json -> to_json is stable.
    EXPECT_EQ(m2->to_json(), json);

    Instruction* mov = m2->functions()[0]->blocks()[0]->instructions()[0];
    ASSERT_EQ(mov->opcode(), Opcode::Mov);
    EXPECT_EQ(mov->type(), Type::V128(LaneKind::F32x4));
    auto* dst2 = dynamic_cast<Constant*>(mov->operands()[0]);
    auto* src2 = dynamic_cast<Constant*>(mov->operands()[1]);
    ASSERT_NE(dst2, nullptr);
    ASSERT_NE(src2, nullptr);
    EXPECT_EQ(dst2->type(), Type::V128(LaneKind::F32x4));
    EXPECT_EQ(dst2->value(), 0u);
    EXPECT_EQ(src2->value(), dup);
    EXPECT_EQ(src2->value_hi(), dup);
}
