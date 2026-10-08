// Vulkan GPUBackend implementation (Phase 3-3 + Phase 4-1 compute dispatch).
#include "switchrecomp/gpu/gpu.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef SR_HAS_VULKAN
#include <vulkan/vulkan.h>

#include "compute_executor.h"
#endif

namespace sr::gpu {

namespace {

#ifdef SR_HAS_VULKAN
// Internal guest GPU method id driving the add1 compute dispatch.
// Placeholder: not a real Maxwell method address. The FIFO word method
// field is 12 bits (see ParseFifoWord), so the requested 0x1D00 placeholder
// is carried on the wire as 0x0D00.
constexpr u16 kMethodComputeAdd1 = 0x0D00;
const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
const char* kDebugUtilsExtName = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

VKAPI_ATTR VkBool32 VKAPI_CALL
DebugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                   VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                   const VkDebugUtilsMessengerCallbackDataEXT* data,
                   void* /*user_data*/) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[vulkan-validation] %s\n",
                     data != nullptr ? data->pMessage : "(null message)");
    }
    return VK_FALSE;
}
#endif

class VulkanBackend final : public GPUBackend {
public:
    ~VulkanBackend() override { Shutdown(); }

    void Initialize() override;
    void Shutdown() override;

    void SubmitCommandBuffer(u64 guest_addr, usize size) override;
    void SubmitCommandBuffer(u64 guest_addr, usize size,
                             MemoryManager& memory) override;

    bool RegisterMethodHandler(u16 method, MethodHandler handler) override;

    bool InitializeSucceeded() const override { return initialized_; }
    std::string InitializeErrorMessage() const override { return init_error_; }

private:
    CommandProcessor processor_;
    bool initialized_ = false;
    std::string init_error_;
    std::string submit_error_;
    // Memory manager for the command buffer currently being processed; used
    // by internal handlers to reach guest data areas. Declared unconditionally
    // because SubmitCommandBuffer sets/clears it even when Vulkan is compiled out.
    MemoryManager* active_memory_ = nullptr;

#ifdef SR_HAS_VULKAN
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;

    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    bool debug_utils_enabled_ = false;

    ComputeExecutor compute_executor_;

    // Record an initialization error and release resources created so far.
    void Fail(const char* what, VkResult result);

    // Register the internal add1 compute method handler.
    void RegisterComputeHandlers();
#endif
};

#ifdef SR_HAS_VULKAN
void VulkanBackend::Fail(const char* what, VkResult result) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), "%s failed (VkResult=%d)", what,
                  static_cast<int>(result));
    init_error_ = msg;
    Shutdown();
}

void VulkanBackend::RegisterComputeHandlers() {
    // A previous Initialize() may have registered the same handler.
    processor_.UnregisterHandler(kMethodComputeAdd1);

    // Long-packet payload convention for method 0x1D00:
    //   word 0: guest address of the u32 data area, low  32 bits
    //   word 1: guest address of the u32 data area, high 32 bits
    //   word 2: element count (must be a multiple of 64)
    // The handler reads the data area, runs the add1 compute shader on the
    // real device, then writes the incremented results back to the area.
    const MethodHandler handler =
        [this](u8 /*subchannel*/, u16 /*method*/, span<const u32> data)
        -> bool {
        if (active_memory_ == nullptr || data.size() < 3) {
            return false;
        }
        const u64 guest_addr =
            static_cast<u64>(data[0]) | (static_cast<u64>(data[1]) << 32);
        const u32 element_count = data[2];
        if (element_count == 0 || element_count % 64 != 0) {
            return false;
        }

        const u64 byte_size = static_cast<u64>(element_count) * sizeof(u32);
        void* host = active_memory_->GuestToHost(guest_addr, byte_size, true);
        if (host == nullptr) {
            return false;
        }

        std::vector<u32> out;
        if (!compute_executor_.RunAdd1(
                element_count,
                span<const u32>(static_cast<const u32*>(host), element_count),
                out)) {
            return false;
        }
        std::memcpy(host, out.data(), static_cast<size_t>(byte_size));
        return true;
    };

    processor_.RegisterHandler(kMethodComputeAdd1, std::move(handler));
}
#endif

void VulkanBackend::Initialize() {
    // Reset any previous initialization attempt.
    Shutdown();
    init_error_.clear();

#ifdef SR_HAS_VULKAN
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SwitchRecomp";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "SwitchRecomp";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instance_ci{
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_ci.pApplicationInfo = &app;

    // Enable the Khronos validation layer only when the loader reports it.
    u32 layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layer_props(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layer_props.data());
    bool validation_available = false;
    for (const VkLayerProperties& lp : layer_props) {
        if (std::strcmp(lp.layerName, kValidationLayerName) == 0) {
            validation_available = true;
            break;
        }
    }
    if (validation_available) {
        instance_ci.enabledLayerCount = 1;
        instance_ci.ppEnabledLayerNames = &kValidationLayerName;

        // Debug utils lets the layer route messages to a callback.
        u32 ext_count = 0;
        vkEnumerateInstanceExtensionProperties(kValidationLayerName,
                                               &ext_count, nullptr);
        std::vector<VkExtensionProperties> ext_props(ext_count);
        vkEnumerateInstanceExtensionProperties(kValidationLayerName,
                                               &ext_count, ext_props.data());
        for (const VkExtensionProperties& ep : ext_props) {
            if (std::strcmp(ep.extensionName, kDebugUtilsExtName) == 0) {
                instance_ci.enabledExtensionCount = 1;
                instance_ci.ppEnabledExtensionNames = &kDebugUtilsExtName;
                debug_utils_enabled_ = true;
                break;
            }
        }
    }

    VkResult r = vkCreateInstance(&instance_ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        Fail("vkCreateInstance", r);
        return;
    }

    if (debug_utils_enabled_) {
        auto create_messenger =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(
                    instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT messenger_ci{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            messenger_ci.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger_ci.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messenger_ci.pfnUserCallback = DebugUtilsCallback;
            if (create_messenger(instance_, &messenger_ci, nullptr,
                                 &debug_messenger_) != VK_SUCCESS) {
                debug_messenger_ = VK_NULL_HANDLE;
            }
        }
    }

    u32 device_count = 0;
    r = vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (r != VK_SUCCESS) {
        Fail("vkEnumeratePhysicalDevices", r);
        return;
    }
    if (device_count == 0) {
        init_error_ = "no Vulkan-capable physical devices found";
        Shutdown();
        return;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    r = vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
    if (r != VK_SUCCESS) {
        Fail("vkEnumeratePhysicalDevices", r);
        return;
    }
    physical_device_ = devices[0];

    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count,
                                             families.data());
    queue_family_ = 0;
    for (u32 i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            queue_family_ = i;
            break;
        }
    }

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_ci.queueFamilyIndex = queue_family_;
    queue_ci.queueCount = 1;
    queue_ci.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_ci.queueCreateInfoCount = 1;
    device_ci.pQueueCreateInfos = &queue_ci;

    r = vkCreateDevice(physical_device_, &device_ci, nullptr, &device_);
    if (r != VK_SUCCESS) {
        Fail("vkCreateDevice", r);
        return;
    }

    // A failing executor is non-fatal for basic backend functionality;
    // its method handler simply stays unregistered.
    if (compute_executor_.Initialize(device_, physical_device_,
                                     queue_family_)) {
        RegisterComputeHandlers();
    }

    initialized_ = true;
#else
    init_error_ =
        "Vulkan unavailable: SwitchRecomp was built without SR_HAS_VULKAN";
#endif
}

void VulkanBackend::Shutdown() {
#ifdef SR_HAS_VULKAN
    active_memory_ = nullptr;

    compute_executor_.Shutdown();

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    physical_device_ = VK_NULL_HANDLE;

    if (debug_messenger_ != VK_NULL_HANDLE) {
        auto destroy_messenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(
                    instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger != nullptr) {
            destroy_messenger(instance_, debug_messenger_, nullptr);
        }
        debug_messenger_ = VK_NULL_HANDLE;
    }
    debug_utils_enabled_ = false;

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    queue_family_ = 0;
#endif
    initialized_ = false;
}

void VulkanBackend::SubmitCommandBuffer(u64 guest_addr, usize size) {
    // The guest address cannot be translated without a MemoryManager.
    submit_error_ = "SubmitCommandBuffer requires a MemoryManager "
                    "(use the 3-argument overload)";
    (void)guest_addr;
    (void)size;
}

void VulkanBackend::SubmitCommandBuffer(u64 guest_addr, usize size,
                                        MemoryManager& memory) {
    void* host = memory.GuestToHost(guest_addr, size, false);
    if (host == nullptr) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "GuestToHost failed for guest address 0x%llX "
                      "(%zu bytes)",
                      static_cast<unsigned long long>(guest_addr), size);
        submit_error_ = msg;
        return;
    }

    const usize word_count = size / sizeof(u32);
    active_memory_ = &memory;
    const Result<usize> processed = processor_.ProcessBuffer(
        span<const u32>(static_cast<const u32*>(host), word_count));
    active_memory_ = nullptr;

    if (!processed.ok()) {
        submit_error_ = processed.error().message;
    } else {
        submit_error_.clear();
    }
}

bool VulkanBackend::RegisterMethodHandler(u16 method, MethodHandler handler) {
    return processor_.RegisterHandler(method, std::move(handler));
}

} // namespace

GPUBackend* CreateVulkanBackend() { return new VulkanBackend(); }

} // namespace sr::gpu
