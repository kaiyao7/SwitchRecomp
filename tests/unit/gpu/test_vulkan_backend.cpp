// GPU-VK-001: Vulkan backend unit tests (Phase 3-3)
#include <gtest/gtest.h>

#include "switchrecomp/gpu/gpu.h"

#include <cstring>
#include <memory>
#include <vector>

using sr::common::span;
using sr::common::u16;
using sr::common::u32;
using sr::common::u64;
using sr::common::u8;
using sr::common::usize;
using sr::gpu::CreateVulkanBackend;
using sr::gpu::GPUBackend;
using sr::gpu::MethodHandler;
using sr::platform::MemProtect;
using sr::runtime::MemoryManager;

// Simplified Maxwell FIFO word layout (see test_command_processor.cpp):
// [11:0] method, [15:13] subchannel, [29:16] count, [31] long flag.
static u32 MakeWord(u16 method, u8 subchannel, u16 count, bool is_long) {
    return (static_cast<u32>(method) & 0x0FFF) |
           ((static_cast<u32>(subchannel) & 0x7) << 13) |
           ((static_cast<u32>(count) & 0x3FFF) << 16) |
           (is_long ? (1u << 31) : 0u);
}

TEST(VulkanBackend, FactoryReturnsInstance) {
    std::unique_ptr<GPUBackend> backend(CreateVulkanBackend());
    ASSERT_NE(backend, nullptr);
    EXPECT_FALSE(backend->InitializeSucceeded());
}

TEST(VulkanBackend, InitializeAndShutdown) {
    std::unique_ptr<GPUBackend> backend(CreateVulkanBackend());
    ASSERT_NE(backend, nullptr);

    backend->Initialize();
    if (!backend->InitializeSucceeded()) {
        // No SDK (built without SR_HAS_VULKAN) or no usable driver at
        // runtime: an explicit error is acceptable, not a failure.
        GTEST_SKIP() << "Vulkan initialization unavailable: "
                     << backend->InitializeErrorMessage();
    }

    EXPECT_TRUE(backend->InitializeSucceeded());
    EXPECT_TRUE(backend->InitializeErrorMessage().empty());

    backend->Shutdown();
    EXPECT_FALSE(backend->InitializeSucceeded());
}

// Vulkan-independent path: method registration + guest command buffer
// submission drives the command processor even without Initialize().
TEST(VulkanBackend, SubmitDispatchesRegisteredHandler) {
    std::unique_ptr<GPUBackend> backend(CreateVulkanBackend());
    ASSERT_NE(backend, nullptr);

    int calls = 0;
    u16 got_method = 0;
    u8 got_subchannel = 0;
    u32 got_value = 0;
    const MethodHandler handler = [&](u8 subchannel, u16 method,
                                      span<const u32> data) {
        ++calls;
        got_subchannel = subchannel;
        got_method = method;
        got_value = data[0];
        return true;
    };
    ASSERT_TRUE(backend->RegisterMethodHandler(0x055, handler));
    // Duplicate registration is rejected.
    EXPECT_FALSE(backend->RegisterMethodHandler(0x055, handler));

    const std::vector<u32> commands = {
        MakeWord(0x055, 2, 0x1234, false),
    };
    const u64 guest_addr = 0x200000;
    const usize byte_size = commands.size() * sizeof(u32);

    MemoryManager memory;
    ASSERT_TRUE(
        memory.MapGuestMemory(guest_addr, 0x1000, MemProtect::ReadWrite));
    void* host = memory.GuestToHost(guest_addr, byte_size, true);
    ASSERT_NE(host, nullptr);
    std::memcpy(host, commands.data(), byte_size);

    backend->SubmitCommandBuffer(guest_addr, byte_size, memory);

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(got_method, 0x055);
    EXPECT_EQ(got_subchannel, 2);
    EXPECT_EQ(got_value, 0x1234u);
}
