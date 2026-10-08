// T-RC-1-01: IR core data structures — unit tests
#include <gtest/gtest.h>

#include "switchrecomp/ir/ir.h"

using namespace sr::ir;

// Build a simple module: one function, two blocks, arithmetic + branch.
TEST(IrBuild, BasicModuleStructure) {
    IRBuilder builder;
    Module module;

    Function* fn = module.AddFunction();
    fn->set_guest_address(0x400000);

    BasicBlock* entry = fn->AddBlock();
    BasicBlock* exit_bb = fn->AddBlock();

    Constant* c1 = builder.CreateConstant(1, Type::U64());
    Constant* c2 = builder.CreateConstant(2, Type::U64());

    Instruction* add = builder.CreateAdd(c1, c2);
    entry->Append(add);

    Instruction* branch = builder.CreateBranch(exit_bb);
    entry->Append(branch);

    Instruction* ret = builder.CreateReturn();
    exit_bb->Append(ret);

    EXPECT_EQ(module.functions().size(), 1u);
    EXPECT_EQ(fn->guest_address(), 0x400000u);
    EXPECT_EQ(fn->blocks().size(), 2u);
    EXPECT_EQ(entry->instructions().size(), 2u);
    EXPECT_EQ(exit_bb->instructions().size(), 1u);
    EXPECT_EQ(add->opcode(), Opcode::Add);
    EXPECT_EQ(add->operands().size(), 2u);
    EXPECT_EQ(ret->opcode(), Opcode::Return);
}

TEST(IrBuild, UniqueValueIds) {
    IRBuilder builder;
    Constant* a = builder.CreateConstant(0, Type::U32());
    Constant* b = builder.CreateConstant(1, Type::U32());
    Instruction* add = builder.CreateAdd(a, b);

    EXPECT_NE(a->id(), b->id());
    EXPECT_NE(a->id(), add->id());
    EXPECT_NE(b->id(), add->id());
    EXPECT_GT(a->id(), 0u);
}

TEST(IrBuild, ConstantValueAndType) {
    IRBuilder builder;
    Constant* c = builder.CreateConstant(0xCAFE, Type::U64());
    EXPECT_EQ(c->value(), 0xCAFEu);
    EXPECT_EQ(c->type().id, TypeId::U64);
}

TEST(IrVerify, ValidModulePasses) {
    IRBuilder builder;
    Module module;
    Function* fn = module.AddFunction();
    BasicBlock* bb = fn->AddBlock();
    Constant* c = builder.CreateConstant(5, Type::U32());
    bb->Append(builder.CreateAdd(c, c));
    bb->Append(builder.CreateReturn());

    EXPECT_TRUE(module.verify());
}

TEST(IrVerify, EmptyModulePasses) {
    Module module;
    EXPECT_TRUE(module.verify());
}

TEST(IrVerify, DuplicateIdFails) {
    IRBuilder builder;
    Module module;
    Function* fn = module.AddFunction();
    BasicBlock* bb = fn->AddBlock();

    Constant* c = builder.CreateConstant(5, Type::U32());
    Instruction* i1 = builder.CreateAdd(c, c);
    Instruction* i2 = builder.CreateSub(c, c);
    // Force a duplicate id.
    i2->set_id(i1->id());

    bb->Append(i1);
    bb->Append(i2);

    EXPECT_FALSE(module.verify());
}

TEST(IrVerify, NullOperandFails) {
    IRBuilder builder;
    Module module;
    Function* fn = module.AddFunction();
    BasicBlock* bb = fn->AddBlock();

    Instruction* i = builder.CreateInstruction(Opcode::Add, Type::U32(),
                                               {nullptr});
    bb->Append(i);

    EXPECT_FALSE(module.verify());
}

TEST(IrVerify, ZeroIdFails) {
    IRBuilder builder;
    Module module;
    Function* fn = module.AddFunction();
    BasicBlock* bb = fn->AddBlock();

    Constant* c = builder.CreateConstant(1, Type::U32());
    Instruction* i = builder.CreateAdd(c, c);
    i->set_id(0); // invalid id
    bb->Append(i);

    EXPECT_FALSE(module.verify());
}

TEST(IrBuild, AllOpcodesConstructible) {
    IRBuilder builder;
    Constant* c = builder.CreateConstant(0, Type::U64());

    // Just verify each factory returns a non-null instruction with correct opcode.
    EXPECT_EQ(builder.CreateAdd(c, c)->opcode(), Opcode::Add);
    EXPECT_EQ(builder.CreateSub(c, c)->opcode(), Opcode::Sub);
    EXPECT_EQ(builder.CreateAnd(c, c)->opcode(), Opcode::And);
    EXPECT_EQ(builder.CreateOr(c, c)->opcode(), Opcode::Or);
    EXPECT_EQ(builder.CreateXor(c, c)->opcode(), Opcode::Xor);
    EXPECT_EQ(builder.CreateNot(c)->opcode(), Opcode::Not);
    EXPECT_EQ(builder.CreateShl(c, c)->opcode(), Opcode::Shl);
    EXPECT_EQ(builder.CreateShr(c, c)->opcode(), Opcode::Shr);
    EXPECT_EQ(builder.CreateSar(c, c)->opcode(), Opcode::Sar);
    EXPECT_EQ(builder.CreateMul(c, c)->opcode(), Opcode::Mul);
    EXPECT_EQ(builder.CreateLoad(c, Type::U64())->opcode(), Opcode::Load);
    EXPECT_EQ(builder.CreateStore(c, c)->opcode(), Opcode::Store);
    EXPECT_EQ(builder.CreateReturn()->opcode(), Opcode::Return);
    EXPECT_EQ(builder.CreateCall(0x1000)->opcode(), Opcode::Call);
    EXPECT_EQ(builder.CreateIndirectBranch(c)->opcode(), Opcode::IndirectBranch);
    EXPECT_EQ(builder.CreateCast(c, Type::U32())->opcode(), Opcode::Cast);
    EXPECT_EQ(builder.CreateSvc(0x80)->opcode(), Opcode::Svc);
}
