#pragma once
/**
 * @file KernelReplayContext.hpp
 * @brief Context struct passed to KernelDescriptor::ReplayFn during
 *        ExecutionProgram replay.
 *
 * Provides all the data a kernel-specific replay handler needs to issue
 * Vulkan commands without coupling the recorder to task-specific concepts.
 */

#include <vulkan/vulkan.h>
#include <lux/engine/function/visibility.h>
#include <cstdint>

namespace lux::cxx { template <typename Key, typename Value, Key Offset> class OffsetAutoSparseSet; }

namespace lux::render
{
    struct RGPhysicalResource;
    using  RGPhysicalResourceTable = lux::cxx::OffsetAutoSparseSet<uint32_t, RGPhysicalResource, 0>;

    struct RGFrameContext;

    /// Context passed to KernelDescriptor::ReplayFn during command replay.
    struct LUX_FUNCTION_PUBLIC KernelReplayContext
    {
        VkCommandBuffer                 cmd;
        const RGFrameContext&           frame_ctx;
        const RGPhysicalResourceTable&  physical_resources;
        VkPipelineLayout                current_layout;

        /// Conditional-draw flag shared with the generic DrawIndexedIndirectCount handler.
        /// A kernel replay function can set this to true to skip the next draw call.
        bool& skip_next_draw;

        /// Resolve an RG resource index to the physical VkBuffer for the current frame.
        /// Returns VK_NULL_HANDLE if not found.
        VkBuffer resolveBuffer(uint32_t resource_idx) const;
    };

} // namespace lux::render
