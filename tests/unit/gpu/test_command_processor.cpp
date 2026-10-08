// GPU-CP-001: command processor FIFO parsing unit tests
#include <gtest/gtest.h>
#include "switchrecomp/gpu/command_processor.h"

#include <vector>

using namespace sr::gpu;
using sr::common::span;
using sr::common::u16;
using sr::common::u32;
using sr::common::u8;

// Build a FIFO word with the simplified layout:
// [11:0] method, [15:13] subchannel, [29:16] count, [31] long flag.
static u32 MakeWord(u16 method, u8 subchannel, u16 count, bool is_long) {
    return (static_cast<u32>(method) & 0x0FFF) |
           ((static_cast<u32>(subchannel) & 0x7) << 13) |
           ((static_cast<u32>(count) & 0x3FFF) << 16) |
           (is_long ? (1u << 31) : 0u);
}

TEST(FifoWord, ParseImmediate) {
    const FifoWord fw = ParseFifoWord(MakeWord(0x123, 2, 0x1234, false));
    EXPECT_EQ(fw.method, 0x123);
    EXPECT_EQ(fw.subchannel, 2);
    EXPECT_EQ(fw.count, 0x1234);
    EXPECT_EQ(fw.opcode, 0);
    EXPECT_FALSE(fw.is_long);
}

TEST(FifoWord, ParseLong) {
    const FifoWord fw = ParseFifoWord(MakeWord(0xABC, 5, 16, true));
    EXPECT_EQ(fw.method, 0xABC);
    EXPECT_EQ(fw.subchannel, 5);
    EXPECT_EQ(fw.count, 16);
    EXPECT_EQ(fw.opcode, 1);
    EXPECT_TRUE(fw.is_long);
}

TEST(FifoWord, ZeroWord) {
    const FifoWord fw = ParseFifoWord(0);
    EXPECT_EQ(fw.method, 0);
    EXPECT_EQ(fw.subchannel, 0);
    EXPECT_EQ(fw.count, 0);
    EXPECT_EQ(fw.opcode, 0);
    EXPECT_FALSE(fw.is_long);
}

TEST(CommandProcessor, RegisterAndList) {
    CommandProcessor cp;
    auto noop = [](u8, u16, span<const u32>) { return true; };
    EXPECT_TRUE(cp.RegisterHandler(0x10, noop));
    EXPECT_TRUE(cp.RegisterHandler(0x20, noop));
    EXPECT_FALSE(cp.RegisterHandler(0x10, noop)); // duplicate

    const auto methods = cp.ListMethods();
    ASSERT_EQ(methods.size(), 2u);
    EXPECT_EQ(methods[0], 0x10);
    EXPECT_EQ(methods[1], 0x20);

    EXPECT_TRUE(cp.UnregisterHandler(0x10));
    EXPECT_FALSE(cp.UnregisterHandler(0x10));
    EXPECT_EQ(cp.ListMethods().size(), 1u);
}

TEST(CommandProcessor, EmptyBuffer) {
    CommandProcessor cp;
    const auto r = cp.ProcessBuffer(span<const u32>{});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 0u);
}

TEST(CommandProcessor, DispatchImmediate) {
    CommandProcessor cp;
    u16 got_method = 0;
    u8 got_sub = 0;
    u32 got_data = 0;
    usize got_count = 0;
    cp.RegisterHandler(0x55, [&](u8 sc, u16 m, span<const u32> data) {
        got_sub = sc;
        got_method = m;
        got_count = data.size();
        got_data = data[0];
        return true;
    });

    u32 buf[] = {MakeWord(0x55, 3, 0x3EEF, false)};
    const auto r = cp.ProcessBuffer(span<const u32>(buf));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 1u);
    EXPECT_EQ(got_method, 0x55);
    EXPECT_EQ(got_sub, 3);
    ASSERT_EQ(got_count, 1u);
    EXPECT_EQ(got_data, 0x3EEF);
}

TEST(CommandProcessor, DispatchLongPacket) {
    CommandProcessor cp;
    std::vector<u32> got;
    cp.RegisterHandler(0x100, [&](u8, u16, span<const u32> data) {
        got.assign(data.begin(), data.end());
        return true;
    });

    u32 buf[] = {
        MakeWord(0x100, 1, 3, true),
        0x11111111, 0x22222222, 0x33333333,
        MakeWord(0x100, 1, 2, true),
        0xAAAAAAAA, 0xBBBBBBBB,
    };
    const auto r = cp.ProcessBuffer(span<const u32>(buf));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 7u);
    // `got` holds the second packet's payload.
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], 0xAAAAAAAA);
    EXPECT_EQ(got[1], 0xBBBBBBBB);
}

TEST(CommandProcessor, FallbackSubchannel) {
    CommandProcessor cp;
    u8 got_sub = 0xFF;
    cp.RegisterHandler(0x77, [&](u8 sc, u16, span<const u32>) {
        got_sub = sc;
        return true;
    });
    // Subchannel field zero in the word -> buffer-level default is used.
    u32 buf[] = {MakeWord(0x77, 0, 0, false)};
    const auto r = cp.ProcessBuffer(span<const u32>(buf), 4);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(got_sub, 4);
}

TEST(CommandProcessor, UnregisteredMethodSkipped) {
    CommandProcessor cp;
    bool called = false;
    cp.RegisterHandler(0x1, [&](u8, u16, span<const u32>) {
        called = true;
        return true;
    });

    u32 buf[] = {
        MakeWord(0x999, 0, 2, true), 0xDEAD, 0xBEEF, // unregistered, skipped
        MakeWord(0x1, 0, 0, false),
    };
    const auto r = cp.ProcessBuffer(span<const u32>(buf));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 4u);
    EXPECT_TRUE(called);
}

TEST(CommandProcessor, TruncatedLongPacketFails) {
    CommandProcessor cp;
    cp.RegisterHandler(0x2, [](u8, u16, span<const u32>) { return true; });

    // Declares 5 payload words but only 2 remain.
    u32 buf[] = {MakeWord(0x2, 0, 5, true), 0x1, 0x2};
    const auto r = cp.ProcessBuffer(span<const u32>(buf));
    EXPECT_FALSE(r.ok());
}

TEST(CommandProcessor, HandlerFailureFails) {
    CommandProcessor cp;
    cp.RegisterHandler(0x3, [](u8, u16, span<const u32>) { return false; });

    u32 buf[] = {MakeWord(0x3, 0, 0, false)};
    const auto r = cp.ProcessBuffer(span<const u32>(buf));
    EXPECT_FALSE(r.ok());
}

TEST(CommandProcessor, LongPacketZeroCount) {
    CommandProcessor cp;
    usize got = 999;
    cp.RegisterHandler(0x4, [&](u8, u16, span<const u32> data) {
        got = data.size();
        return true;
    });
    u32 buf[] = {MakeWord(0x4, 0, 0, true)};
    const auto r = cp.ProcessBuffer(span<const u32>(buf));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 1u);
    EXPECT_EQ(got, 0u);
}
