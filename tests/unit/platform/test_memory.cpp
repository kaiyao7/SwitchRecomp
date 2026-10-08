// RT-INIT-001: platform memory unit tests
#include <gtest/gtest.h>
#include "switchrecomp/platform/memory.h"

using namespace sr::platform;

TEST(PlatformMemory, PageSizePositive) {
    EXPECT_GT(PageSize(), 0u);
}

TEST(PlatformMemory, AllocPagesWriteRead) {
    usize ps = PageSize();
    void* p = AllocPages(ps, MemProtect::ReadWrite);
    ASSERT_NE(p, nullptr);

    auto* bytes = static_cast<unsigned char*>(p);
    for (usize i = 0; i < ps; ++i) {
        bytes[i] = static_cast<unsigned char>(i & 0xFF);
    }
    for (usize i = 0; i < ps; ++i) {
        EXPECT_EQ(bytes[i], static_cast<unsigned char>(i & 0xFF));
    }

    FreePages(p, ps);
}

TEST(PlatformMemory, ProtectPagesReturnsTrue) {
    usize ps = PageSize();
    void* p = AllocPages(ps, MemProtect::ReadWrite);
    ASSERT_NE(p, nullptr);

    // Change to read-only; just verify the call succeeds.
    EXPECT_TRUE(ProtectPages(p, ps, MemProtect::Read));

    // Restore to RW so we can free safely.
    EXPECT_TRUE(ProtectPages(p, ps, MemProtect::ReadWrite));
    FreePages(p, ps);
}
