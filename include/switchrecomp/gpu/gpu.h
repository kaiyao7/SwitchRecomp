#pragma once

#include "switchrecomp/common/types.h"
#include "switchrecomp/gpu/command_processor.h"
#include "switchrecomp/runtime/memory_manager.h"

#include <string>

namespace sr::gpu {

using sr::common::u16;
using sr::common::u64;
using sr::common::usize;
using sr::runtime::MemoryManager;

// Abstract GPU backend interface.
class GPUBackend {
public:
    virtual ~GPUBackend() = default;

    virtual void Initialize() = 0;
    virtual void Shutdown() = 0;

    // Submit a GPU command buffer located at guest address.
    virtual void SubmitCommandBuffer(u64 guest_addr, usize size) = 0;

    // --- Phase 3-3 additive API -------------------------------------
    // Default implementations keep out-of-tree GPUBackend subclasses
    // source-compatible; the Vulkan backend overrides all of them.

    // Submit a guest command buffer: translate guest_addr via MemoryManager,
    // then parse/dispatch it through the backend's CommandProcessor.
    virtual void SubmitCommandBuffer(u64 guest_addr, usize size,
                                     MemoryManager& memory) {
        (void)guest_addr;
        (void)size;
        (void)memory;
    }

    // Register a handler for a guest GPU method. Returns false when the
    // method already has a registered handler.
    virtual bool RegisterMethodHandler(u16 method, MethodHandler handler) {
        (void)method;
        (void)handler;
        return false;
    }

    // Result of the most recent Initialize() call.
    virtual bool InitializeSucceeded() const { return false; }
    virtual std::string InitializeErrorMessage() const { return {}; }
};

// Create a Vulkan backend. Always non-null; when built without Vulkan
// support (SR_HAS_VULKAN undefined) Initialize() reports an explicit error.
GPUBackend* CreateVulkanBackend();

} // namespace sr::gpu
