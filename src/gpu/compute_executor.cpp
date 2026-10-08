// Internal GPU module: minimal Vulkan compute pipeline (Phase 4-1).
#include "compute_executor.h"

#include <cstring>

#ifdef SR_HAS_VULKAN

#ifdef SR_GPU_HAS_SHADERS
#include "generated/add1_spv.h"
#endif

namespace sr::gpu {

namespace {

// Find a device memory type matching `type_bits` and all `wanted` flags.
// Returns UINT32_MAX when none matches.
u32 FindMemoryType(VkPhysicalDevice physical_device, u32 type_bits,
                   VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &props);
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0 &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return UINT32_MAX;
}

} // namespace

bool ComputeExecutor::Initialize(VkDevice device,
                                 VkPhysicalDevice physical_device,
                                 u32 queue_family) {
    Shutdown();
    device_ = device;
    physical_device_ = physical_device;
    queue_family_ = queue_family;
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    // Command pool + a single reusable command buffer.
    VkCommandPoolCreateInfo pool_ci{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex = queue_family_;
    if (vkCreateCommandPool(device_, &pool_ci, nullptr, &command_pool_) !=
        VK_SUCCESS) {
        Shutdown();
        return false;
    }

    VkCommandBufferAllocateInfo cb_ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_ai.commandPool = command_pool_;
    cb_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &cb_ai, &command_buffer_) !=
        VK_SUCCESS) {
        Shutdown();
        return false;
    }

    VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_, &fence_ci, nullptr, &submit_fence_) !=
        VK_SUCCESS) {
        Shutdown();
        return false;
    }

    // Descriptor set layout: set 0 binding 0 = one storage buffer.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dsl_ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl_ci.bindingCount = 1;
    dsl_ci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &dsl_ci, nullptr,
                                    &descriptor_set_layout_) != VK_SUCCESS) {
        Shutdown();
        return false;
    }

    VkPipelineLayoutCreateInfo pl_ci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &descriptor_set_layout_;
    if (vkCreatePipelineLayout(device_, &pl_ci, nullptr, &pipeline_layout_) !=
        VK_SUCCESS) {
        Shutdown();
        return false;
    }

#ifdef SR_GPU_HAS_SHADERS
    VkShaderModuleCreateInfo sm_ci{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm_ci.codeSize = kAdd1SpvSize;
    sm_ci.pCode = kAdd1Spv;
    if (vkCreateShaderModule(device_, &sm_ci, nullptr, &shader_module_) !=
        VK_SUCCESS) {
        Shutdown();
        return false;
    }

    VkComputePipelineCreateInfo cp_ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp_ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp_ci.stage.module = shader_module_;
    cp_ci.stage.pName = "main";
    cp_ci.layout = pipeline_layout_;
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cp_ci, nullptr,
                                 &pipeline_) != VK_SUCCESS) {
        Shutdown();
        return false;
    }
#endif

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dp_ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp_ci.maxSets = 1;
    dp_ci.poolSizeCount = 1;
    dp_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &dp_ci, nullptr, &descriptor_pool_) !=
        VK_SUCCESS) {
        Shutdown();
        return false;
    }

    VkDescriptorSetAllocateInfo ds_ai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ds_ai.descriptorPool = descriptor_pool_;
    ds_ai.descriptorSetCount = 1;
    ds_ai.pSetLayouts = &descriptor_set_layout_;
    if (vkAllocateDescriptorSets(device_, &ds_ai, &descriptor_set_) !=
        VK_SUCCESS) {
        Shutdown();
        return false;
    }

    return true;
}

bool ComputeExecutor::EnsureBufferCapacity(u32 element_count) {
    if (element_count <= capacity_elements_) {
        return true;
    }

    if (buffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
    }
    if (buffer_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer_memory_, nullptr);
        buffer_memory_ = VK_NULL_HANDLE;
    }
    capacity_elements_ = 0;

    VkBufferCreateInfo buf_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buf_ci.size = VkDeviceSize(element_count) * sizeof(u32);
    buf_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &buf_ci, nullptr, &buffer_) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements(device_, buffer_, &reqs);

    const u32 memory_type = FindMemoryType(
        physical_device_, reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX) {
        vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc_ci{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_ci.allocationSize = reqs.size;
    alloc_ci.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device_, &alloc_ci, nullptr, &buffer_memory_) !=
        VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindBufferMemory(device_, buffer_, buffer_memory_, 0) !=
        VK_SUCCESS) {
        vkFreeMemory(device_, buffer_memory_, nullptr);
        buffer_memory_ = VK_NULL_HANDLE;
        vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
        return false;
    }

    capacity_elements_ = element_count;
    return true;
}

bool ComputeExecutor::RunAdd1(u32 element_count, span<const u32> in_data,
                              std::vector<u32>& out_data) {
    if (device_ == VK_NULL_HANDLE || element_count == 0 ||
        element_count % 64 != 0 || in_data.size() < element_count) {
        return false;
    }
#ifndef SR_GPU_HAS_SHADERS
    // Embedded SPIR-V unavailable (no shader compiler and no prebuilt
    // artifacts at configure time).
    return false;
#else
    if (!EnsureBufferCapacity(element_count)) {
        return false;
    }

    const VkDeviceSize bytes = VkDeviceSize(element_count) * sizeof(u32);

    // Upload input through the coherent mapped storage buffer.
    void* mapped = nullptr;
    if (vkMapMemory(device_, buffer_memory_, 0, bytes, 0, &mapped) !=
        VK_SUCCESS) {
        return false;
    }
    std::memcpy(mapped, in_data.data(), static_cast<size_t>(bytes));
    vkUnmapMemory(device_, buffer_memory_);

    VkDescriptorBufferInfo buffer_info{buffer_, 0, bytes};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_set_;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    if (vkResetCommandBuffer(command_buffer_, 0) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferBeginInfo begin_ci{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_ci.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command_buffer_, &begin_ci) != VK_SUCCESS) {
        return false;
    }
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline_);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0, 1, &descriptor_set_, 0,
                            nullptr);
    vkCmdDispatch(command_buffer_, element_count / 64, 1, 1);
    if (vkEndCommandBuffer(command_buffer_) != VK_SUCCESS) {
        return false;
    }

    vkResetFences(device_, 1, &submit_fence_);
    VkSubmitInfo submit_ci{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_ci.commandBufferCount = 1;
    submit_ci.pCommandBuffers = &command_buffer_;
    if (vkQueueSubmit(queue_, 1, &submit_ci, submit_fence_) != VK_SUCCESS) {
        return false;
    }
    if (vkWaitForFences(device_, 1, &submit_fence_, VK_TRUE,
                        30000000000ULL) != VK_SUCCESS) {
        return false;
    }

    // Read results back through the coherent mapped storage buffer.
    mapped = nullptr;
    if (vkMapMemory(device_, buffer_memory_, 0, bytes, 0, &mapped) !=
        VK_SUCCESS) {
        return false;
    }
    out_data.resize(element_count);
    std::memcpy(out_data.data(), mapped, static_cast<size_t>(bytes));
    vkUnmapMemory(device_, buffer_memory_);
    return true;
#endif
}

void ComputeExecutor::Shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    descriptor_set_ = VK_NULL_HANDLE;
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (shader_module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, shader_module_, nullptr);
        shader_module_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
        descriptor_set_layout_ = VK_NULL_HANDLE;
    }
    if (submit_fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, submit_fence_, nullptr);
        submit_fence_ = VK_NULL_HANDLE;
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    command_buffer_ = VK_NULL_HANDLE;
    if (buffer_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer_memory_, nullptr);
        buffer_memory_ = VK_NULL_HANDLE;
    }
    if (buffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
    }
    capacity_elements_ = 0;

    queue_ = VK_NULL_HANDLE;
    queue_family_ = 0;
    physical_device_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

} // namespace sr::gpu

#endif // SR_HAS_VULKAN
