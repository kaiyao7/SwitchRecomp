// Phase 4-1: end-to-end GPU compute dispatch.
//
// A guest FIFO command (long packet targeting internal method 0x1D00) is
// submitted from guest memory through VulkanBackend -> CommandProcessor ->
// handler -> a real Vulkan compute pipeline. The handler reads an input
// area in guest memory, runs the add1 shader, and writes the results back
// to the same guest area; the test then verifies every element in guest
// memory was incremented by one.
#include <gtest/gtest.h>

#include "switchrecomp/gpu/gpu.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using sr::common::u16;
using sr::common::u32;
using sr::common::u64;
using sr::common::u8;
using sr::gpu::CreateVulkanBackend;
using sr::gpu::GPUBackend;
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

TEST(ComputeDispatch, GuestCommandIncrementsBufferByOne) {
#ifndef SR_HAS_VULKAN
    GTEST_SKIP() << "built without Vulkan support";
#else
    std::unique_ptr<GPUBackend> backend(CreateVulkanBackend());
    ASSERT_NE(backend, nullptr);
    backend->Initialize();
    if (!backend->InitializeSucceeded()) {
        GTEST_SKIP() << "Vulkan initialization unavailable: "
                     << backend->InitializeErrorMessage();
    }
#ifndef SR_GPU_HAS_SHADERS
    GTEST_SKIP() << "embedded compute shaders unavailable "
                    "(no shader compiler and no prebuilt artifacts)";
#endif

    // Guest layout: one mapped RW region holding the command buffer and
    // the input/output data area.
    constexpr u64 kRegionBase = 0x00010000;
    constexpr u64 kRegionSize = 0x00010000;
    const u64 cmd_addr = kRegionBase;
    const u64 data_addr = kRegionBase + 0x1000;
    constexpr u32 kElementCount = 64;

    MemoryManager memory;
    ASSERT_TRUE(memory.MapGuestMemory(kRegionBase, kRegionSize,
                                      MemProtect::ReadWrite));

    // Prepare input data area in guest memory: 0, 1, ..., 63.
    u32* values = static_cast<u32*>(memory.GuestToHost(
        data_addr, u64(kElementCount) * sizeof(u32), true));
    ASSERT_NE(values, nullptr);
    for (u32 i = 0; i < kElementCount; ++i) {
        values[i] = i;
    }

    // Build the guest command buffer: long packet for method 0x1D00 with
    // payload [data_addr low, data_addr high, element count].
    // Internal add1 placeholder 0x1D00 carried in the 12-bit method field.
    const std::vector<u32> commands = {
        MakeWord(0x0D00, 0, 3, true),
        static_cast<u32>(data_addr),
        static_cast<u32>(data_addr >> 32),
        kElementCount,
    };
    void* cmd_host = memory.GuestToHost(
        cmd_addr, commands.size() * sizeof(u32), true);
    ASSERT_NE(cmd_host, nullptr);
    std::memcpy(cmd_host, commands.data(),
                commands.size() * sizeof(u32));

    // The submit synchronously parses/dispatches the guest command and the
    // handler blocks on the GPU fence, so timing the submit measures the
    // real device round trip.
    const auto t0 = std::chrono::steady_clock::now();
    backend->SubmitCommandBuffer(cmd_addr,
                                 commands.size() * sizeof(u32), memory);
    const auto t1 = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Verify the results written back to guest memory: i -> i + 1.
    for (u32 i = 0; i < kElementCount; ++i) {
        EXPECT_EQ(values[i], i + 1u) << "element " << i;
    }

    std::printf(
        "[ComputeDispatch] GPU add1 dispatch: %.3f ms; first values: "
        "%u %u %u %u %u %u %u %u\n",
        elapsed_ms, values[0], values[1], values[2], values[3], values[4],
        values[5], values[6], values[7]);

    // Explicit shutdown must be a no-crash no-op path; the destructor will
    // call Shutdown() again.
    backend->Shutdown();
#endif
}
