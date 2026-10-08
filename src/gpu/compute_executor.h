// Internal GPU module: minimal Vulkan compute pipeline used to execute
// guest-dispatched compute workloads (Phase 4-1). Not a public header.
#pragma once

#include "switchrecomp/common/span.h"
#include "switchrecomp/common/types.h"

#include <vector>

#ifdef SR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace sr::gpu {

using sr::common::span;
using sr::common::u32;

// Owns a minimal compute pipeline running the embedded add1 shader:
// every uint32 element of the bound storage buffer is incremented by one.
//
// The Vulkan device/instance are owned by VulkanBackend; the executor only
// borrows them and must be Shutdown() before the device is destroyed.
class ComputeExecutor {
public:
    ComputeExecutor() = default;
    ~ComputeExecutor() { Shutdown(); }

    ComputeExecutor(const ComputeExecutor&) = delete;
    ComputeExecutor& operator=(const ComputeExecutor&) = delete;

#ifdef SR_HAS_VULKAN
    // Creates command pool/buffer, descriptor objects, the add1 compute
    // pipeline and acquires the queue. Returns false on any failure.
    bool Initialize(VkDevice device, VkPhysicalDevice physical_device,
                    u32 queue_family);
#else
    // Out-of-line translation unit is omitted entirely without Vulkan;
    // keeping the symbols absent prevents accidental use.
#endif

    // Runs one add1 dispatch:
    //   element_count - number of u32 elements (must be a multiple of 64)
    //   in_data       - element_count input values copied to the GPU buffer
    //   out_data      - receives element_count result values after readback
    // Returns false on validation failure or any Vulkan error.
    bool RunAdd1(u32 element_count, span<const u32> in_data,
                 std::vector<u32>& out_data);

    // Destroys every owned Vulkan object after waiting for device idle.
    // Safe to call multiple times.
    void Shutdown();

private:
#ifdef SR_HAS_VULKAN
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence submit_fence_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkShaderModule shader_module_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    // Persistent storage buffer, grown on demand. HOST_VISIBLE |
    // HOST_COHERENT so input/output can be mapped directly.
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory_ = VK_NULL_HANDLE;
    u32 capacity_elements_ = 0;

    bool EnsureBufferCapacity(u32 element_count);
#endif
};

} // namespace sr::gpu
