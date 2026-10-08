// RT-INIT-001: SVC dispatch unit tests
#include <gtest/gtest.h>
#include "switchrecomp/runtime/runtime.h"

using namespace sr::runtime;

TEST(Svc, SetHeapSizeReturnsHeapBase) {
    GuestContext ctx{};
    ctx.x[0] = 0;
    HandleSvc(&ctx, 0x01);
    EXPECT_EQ(ctx.x[0], 0x100000u);
}

TEST(Svc, UnknownSvcDoesNotCrash) {
    GuestContext ctx{};
    // Just verify dispatch doesn't crash / throw for an unknown id.
    EXPECT_NO_THROW(HandleSvc(&ctx, 0xFF));
    EXPECT_NO_THROW(HandleSvc(&ctx, 0x1234));
}
