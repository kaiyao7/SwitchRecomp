// Smoke test: verifies the build links and headers are self-consistent.
#include <gtest/gtest.h>

#include "switchrecomp/common/types.h"
#include "switchrecomp/common/result.h"
#include "switchrecomp/ir/ir.h"
#include "switchrecomp/platform/memory.h"

TEST(Smoke, Types) {
    sr::common::u64 v = 0xDEAD'BEEF'CAFE'BABEull;
    EXPECT_EQ(v, 0xDEADBEEFCAFEBABEull);
}

TEST(Smoke, ResultOk) {
    sr::common::Result<int> r{42};
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 42);
}

TEST(Smoke, ResultErr) {
    sr::common::Result<int> r{sr::common::Error{"boom"}};
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error().message, "boom");
}

TEST(Smoke, PlatformPageSize) {
    EXPECT_GT(sr::platform::PageSize(), 0u);
}

TEST(Smoke, IrModule) {
    sr::ir::Module m;
    EXPECT_TRUE(m.verify());
}
