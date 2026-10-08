// RT-THREAD: thread / synchronization SVC HLE unit tests.
//
// Drives the runtime's thread HLE handlers through the public HandleSvc
// dispatch (which routes to the process-global HleServiceRegistry). Each
// CreateThread starts a host worker that increments ctx->shared_counter
// 10000 times under the runtime mutex; WaitSynchronization joins; GetResult
// exposes the counter via x0.
#include <gtest/gtest.h>

#include "switchrecomp/runtime/runtime.h"

using namespace sr::runtime;

namespace {

// SVC ids used by the thread HLE (see svc_thread.cpp).
constexpr u32 kSvcCreateThread = 0x0A;
constexpr u32 kSvcWaitSync = 0x0B;
constexpr u32 kSvcGetResult = 0x0C;
constexpr u32 kSvcStartThread = 0x22;
constexpr u32 kSvcExitThread = 0x23;
constexpr u32 kSvcSleepThread = 0x24;

// Each worker increments the shared counter this many times; must match
// kWorkerIncrements in svc_thread.cpp.
constexpr u64 kWorkerIncrements = 10000;

} // namespace

// Single worker: create -> wait -> get result should yield 10000.
TEST(SvcThread, SingleWorkerCounterIs10000) {
    GuestContext ctx{};
    HandleSvc(&ctx, kSvcCreateThread);
    const u64 handle = ctx.x[0];
    EXPECT_NE(handle, 0u);

    HandleSvc(&ctx, kSvcWaitSync);   // join (reads x0 = handle)
    EXPECT_EQ(ctx.x[0], 0u);         // success

    HandleSvc(&ctx, kSvcGetResult);
    EXPECT_EQ(ctx.x[0], kWorkerIncrements);
    EXPECT_EQ(ctx.shared_counter, kWorkerIncrements);
}

// Two workers, each 10000 increments, joined individually -> 20000.
TEST(SvcThread, TwoWorkersCounterIs20000) {
    GuestContext ctx{};

    HandleSvc(&ctx, kSvcCreateThread);
    const u64 h1 = ctx.x[0];
    HandleSvc(&ctx, kSvcCreateThread);
    const u64 h2 = ctx.x[0];
    EXPECT_NE(h1, h2);

    ctx.x[0] = h1;
    HandleSvc(&ctx, kSvcWaitSync);
    ctx.x[0] = h2;
    HandleSvc(&ctx, kSvcWaitSync);

    HandleSvc(&ctx, kSvcGetResult);
    EXPECT_EQ(ctx.x[0], 2u * kWorkerIncrements);
    EXPECT_EQ(ctx.shared_counter, 2u * kWorkerIncrements);
}

// CreateThread must populate thread_handle too (mirrors x0).
TEST(SvcThread, CreateThreadSetsThreadHandleField) {
    GuestContext ctx{};
    HandleSvc(&ctx, kSvcCreateThread);
    const u64 handle = ctx.x[0];
    EXPECT_EQ(ctx.thread_handle, handle);
    ctx.x[0] = handle;
    HandleSvc(&ctx, kSvcWaitSync);
}

// StartThread / ExitThread / SleepThread dispatch without crashing.
TEST(SvcThread, AuxSvcsDispatchWithoutCrash) {
    GuestContext ctx{};
    EXPECT_NO_THROW(HandleSvc(&ctx, kSvcStartThread));
    EXPECT_NO_THROW(HandleSvc(&ctx, kSvcExitThread));
    EXPECT_NO_THROW(HandleSvc(&ctx, kSvcSleepThread));
}

// WaitSynchronization on an unknown handle is a no-op that reports success.
TEST(SvcThread, WaitSyncUnknownHandleReportsSuccess) {
    GuestContext ctx{};
    ctx.x[0] = 0xDEADBEEFu; // never created by AllocateHandle
    EXPECT_NO_THROW(HandleSvc(&ctx, kSvcWaitSync));
    EXPECT_EQ(ctx.x[0], 0u);
}
