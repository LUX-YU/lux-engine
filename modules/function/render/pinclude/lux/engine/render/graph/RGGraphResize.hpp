#pragma once
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/graph/PhysicalResourceAllocator.hpp>
#include <lux/engine/function/visibility.h>
#include <vulkan/vulkan.h>
#include <functional>

namespace lux::render
{
    class RGVulkanRecorder;

    /**
     * @brief Resize (or initially allocate) per-view physical resources.
     * 
     * The compiled graph is treated as immutable.  RELATIVE_MODE texture sizes
     * are resolved by the allocator using the supplied extent — the graph
     * descriptions are NOT mutated.
     * 
     * @param graph         The compiled graph (read-only)
     * @param new_extent    The target resolution
     * @param state         Per-view resource state to update
     * @param recorder      The Vulkan recorder (manages image views / record context)
     * @param allocator     GPU resource allocator
     * @param frames_in_flight Number of frames in flight
     * @param on_resources_allocated  Optional callback after allocation
     * @param on_old_state_retired Optional callback to retire previous state.
     *                             If null, previous state is deallocated immediately.
     * @param update_mask   External resource group mask (default: GROUP_SWAPCHAIN)
     * @return true on success
     */
    LUX_FUNCTION_PUBLIC bool ResizeViewResources(
        const RGCompiledGraph&          graph,
        VkExtent2D                      new_extent,
        RGResourceState&                state,
        RGVulkanRecorder&               recorder,
        PhysicalResourceAllocator&      allocator,
        uint32_t                        frames_in_flight,
        std::function<void(RGPhysicalResourceTable&)> on_resources_allocated = nullptr,
        std::function<void(RGResourceState&&)>        on_old_state_retired = nullptr,
        uint32_t                        update_mask = static_cast<uint32_t>(RGUpdateGroup::GROUP_SWAPCHAIN));

} // namespace lux::render
