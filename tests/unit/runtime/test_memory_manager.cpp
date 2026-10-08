// RT-INIT-001: runtime memory manager unit tests
#include <gtest/gtest.h>
#include "switchrecomp/runtime/memory_manager.h"

using namespace sr::runtime;
using sr::platform::MemProtect;

TEST(MemoryManager, MapAndTranslate) {
    MemoryManager mm;
    ASSERT_TRUE(mm.MapGuestMemory(0x100000, 4096, MemProtect::ReadWrite));

    void* host = mm.GuestToHost(0x100000, 4, false);
    ASSERT_NE(host, nullptr);

    // Write data and read it back.
    auto* p = static_cast<unsigned char*>(host);
    p[0] = 0xDE;
    p[1] = 0xAD;
    p[2] = 0xBE;
    p[3] = 0xEF;

    void* host2 = mm.GuestToHost(0x100000, 4, false);
    ASSERT_NE(host2, nullptr);
    auto* p2 = static_cast<unsigned char*>(host2);
    EXPECT_EQ(p2[0], 0xDE);
    EXPECT_EQ(p2[1], 0xAD);
    EXPECT_EQ(p2[2], 0xBE);
    EXPECT_EQ(p2[3], 0xEF);

    mm.UnmapGuestMemory(0x100000, 4096);
}

TEST(MemoryManager, UnmappedReturnsNull) {
    MemoryManager mm;
    EXPECT_EQ(mm.GuestToHost(0x200000, 4, false), nullptr);
}

TEST(MemoryManager, OffsetTranslation) {
    MemoryManager mm;
    ASSERT_TRUE(mm.MapGuestMemory(0x100000, 4096, MemProtect::ReadWrite));

    void* base = mm.GuestToHost(0x100000, 1, false);
    void* off = mm.GuestToHost(0x100000 + 64, 1, false);
    ASSERT_NE(base, nullptr);
    ASSERT_NE(off, nullptr);
    EXPECT_EQ(static_cast<char*>(off) - static_cast<char*>(base), 64);

    mm.UnmapGuestMemory(0x100000, 4096);
}
