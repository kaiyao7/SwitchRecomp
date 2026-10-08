// HLE service registry unit tests + SVC table / MemoryManager integration.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "switchrecomp/runtime/runtime.h"
#include "runtime/hle_service.h"
#include "switchrecomp/runtime/memory_manager.h"

using namespace sr::runtime;
using sr::platform::MemProtect;

namespace {

constexpr u64 kX1Marker = 0x123456789abcdef0ULL;
constexpr u64 kX2Marker = 0x00000000cafebabeULL;

void HandlerWriteX1(GuestContext* ctx) {
    ctx->x[1] = kX1Marker;
}

void HandlerWriteX2(GuestContext* ctx) {
    ctx->x[2] = kX2Marker;
}

} // namespace

// ---- Registry basics (local instance, isolated from global state) ----

TEST(HleService, RegisterAndInvokeModifiesContext) {
    HleServiceRegistry registry;
    GuestContext ctx{};

    ASSERT_TRUE(registry.Register("WriteX1", &HandlerWriteX1));
    EXPECT_TRUE(registry.Invoke("WriteX1", &ctx));
    EXPECT_EQ(ctx.x[1], kX1Marker);
}

TEST(HleService, ContainsAndListServices) {
    HleServiceRegistry registry;

    EXPECT_TRUE(registry.ListServices().empty());

    ASSERT_TRUE(registry.Register("Bravo", &HandlerWriteX1));
    ASSERT_TRUE(registry.Register("Alpha", &HandlerWriteX1));
    ASSERT_TRUE(registry.Register("Charlie", &HandlerWriteX2));

    EXPECT_TRUE(registry.Contains("Alpha"));
    EXPECT_TRUE(registry.Contains("Bravo"));
    EXPECT_TRUE(registry.Contains("Charlie"));
    EXPECT_FALSE(registry.Contains("Missing"));

    // Names come back sorted because the backing container is std::map.
    const std::vector<std::string> expected{"Alpha", "Bravo", "Charlie"};
    EXPECT_EQ(registry.ListServices(), expected);
}

TEST(HleService, DuplicateRegistrationKeepsExistingHandler) {
    HleServiceRegistry registry;

    ASSERT_TRUE(registry.Register("Dup", &HandlerWriteX1));
    // Duplicate name is rejected and must not replace the handler.
    EXPECT_FALSE(registry.Register("Dup", &HandlerWriteX2));

    GuestContext ctx{};
    ASSERT_TRUE(registry.Invoke("Dup", &ctx));
    EXPECT_EQ(ctx.x[1], kX1Marker);
    EXPECT_EQ(ctx.x[2], 0u);
}

TEST(HleService, NullHandlerRejected) {
    HleServiceRegistry registry;
    EXPECT_FALSE(registry.Register("Null", nullptr));
    EXPECT_FALSE(registry.Contains("Null"));
}

TEST(HleService, InvokeMissingReturnsFalseAndDoesNotCrash) {
    HleServiceRegistry registry;
    GuestContext ctx{};

    EXPECT_NO_THROW(EXPECT_FALSE(registry.Invoke("NoSuchService", &ctx)));
}

TEST(HleService, UnregisterStopsService) {
    HleServiceRegistry registry;
    GuestContext ctx{};

    ASSERT_TRUE(registry.Register("Temp", &HandlerWriteX1));
    EXPECT_TRUE(registry.Contains("Temp"));
    EXPECT_TRUE(registry.Unregister("Temp"));

    EXPECT_FALSE(registry.Contains("Temp"));
    EXPECT_FALSE(registry.Invoke("Temp", &ctx));
    // Unregistering an unknown name also fails.
    EXPECT_FALSE(registry.Unregister("Temp"));
}

// ---- Process-global registry + extern "C" ABI ----

TEST(HleService, GlobalCWrapperInvokeAndCleanup) {
    static constexpr const char* kName = "TestHleService_CWrapper";
    HleServiceRegistry& global = GlobalHleServices();
    global.Unregister(kName); // defensive: start from a clean state

    GuestContext ctx{};
    EXPECT_FALSE(sr_hle_invoke(kName, &ctx));
    EXPECT_FALSE(sr_hle_invoke(nullptr, &ctx));

    ASSERT_TRUE(sr_hle_register(kName, &HandlerWriteX1));
    EXPECT_TRUE(global.Contains(kName));
    ASSERT_TRUE(sr_hle_invoke(kName, &ctx));
    EXPECT_EQ(ctx.x[1], kX1Marker);

    EXPECT_TRUE(global.Unregister(kName));
    EXPECT_FALSE(sr_hle_invoke(kName, &ctx));
}

// ---- SVC table dispatched through the registry ----

TEST(HleService, SvcSetHeapSizeViaRegistry) {
    GuestContext ctx{};
    ctx.x[0] = 0;
    HandleSvc(&ctx, 0x01);
    EXPECT_EQ(ctx.x[0], 0x100000u);

    // Built-in services must be present in the global registry after dispatch.
    EXPECT_TRUE(GlobalHleServices().Contains("SetHeapSize"));
}

TEST(HleService, BuiltinSvcsDispatchedByName) {
    // Mutex is a no-op but must dispatch without throwing.
    GuestContext ctx{};
    EXPECT_NO_THROW(HandleSvc(&ctx, 0x16));
    // Unknown SVC ids keep the warning behavior and don't crash.
    EXPECT_NO_THROW(HandleSvc(&ctx, 0xFF));
    EXPECT_NO_THROW(HandleSvc(&ctx, 0x1234));
}

// ---- MemoryManager query enhancements ----

TEST(HleService, QueryMappingReportsInfo) {
    MemoryManager mm;
    ASSERT_TRUE(mm.MapGuestMemory(0x400000, 4096, MemProtect::Read));

    MappingInfo info{};
    // An address inside the mapping reports start / size / permissions.
    ASSERT_TRUE(mm.QueryMapping(0x400100, info));
    EXPECT_EQ(info.guest_addr, 0x400000u);
    EXPECT_EQ(info.size, 4096u);
    EXPECT_EQ(info.prot, MemProtect::Read);

    // Unmapped address fails and leaves `out` untouched.
    MappingInfo untouched{0x11111111u, 0x22222222u, MemProtect::ReadWriteExec};
    EXPECT_FALSE(mm.QueryMapping(0x900000, untouched));
    EXPECT_EQ(untouched.guest_addr, 0x11111111u);
    EXPECT_EQ(untouched.size, 0x22222222u);
    EXPECT_EQ(untouched.prot, MemProtect::ReadWriteExec);

    // Address immediately past the mapping end is not covered.
    EXPECT_FALSE(mm.QueryMapping(0x401000, info));

    mm.UnmapGuestMemory(0x400000, 4096);
    EXPECT_FALSE(mm.QueryMapping(0x400100, info));
}

TEST(HleService, TranslateDistinguishesUnmappedFromPermissionDenied) {
    MemoryManager mm;
    ASSERT_TRUE(mm.MapGuestMemory(0x500000, 4096, MemProtect::Read));

    void* host = nullptr;

    // Mapped read-only: write is a permission failure, not unmapped.
    EXPECT_EQ(mm.Translate(0x500000, 4, true, &host),
              TranslateResult::PermissionDenied);
    EXPECT_EQ(host, nullptr);

    // Nothing at this address at all.
    EXPECT_EQ(mm.Translate(0x600000, 4, true, &host),
              TranslateResult::Unmapped);

    // Read access succeeds and yields a host pointer.
    host = nullptr;
    EXPECT_EQ(mm.Translate(0x500000, 4, false, &host),
              TranslateResult::Success);
    EXPECT_NE(host, nullptr);

    // A range extending past the mapping is unmapped.
    EXPECT_EQ(mm.Translate(0x500f00, 4096, false, &host),
              TranslateResult::Unmapped);

    // The legacy API stays consistent with the detailed result.
    EXPECT_NE(mm.GuestToHost(0x500000, 4, false), nullptr);
    EXPECT_EQ(mm.GuestToHost(0x500000, 4, true), nullptr);
    EXPECT_EQ(mm.GuestToHost(0x600000, 4, false), nullptr);

    mm.UnmapGuestMemory(0x500000, 4096);
}
