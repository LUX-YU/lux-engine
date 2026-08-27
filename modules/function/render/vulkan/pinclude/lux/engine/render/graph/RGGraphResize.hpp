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
     * @param on_resources_allocated  Callback after allocation, or nullptr.
     * @param on_old_state_retired How the previous state is released.
     *
     *        ⚠️ **No default on purpose.** Passing nullptr deallocates the old
     *        state IMMEDIATELY, and frames N-1/N-2 still in flight may be
     *        referencing its image views and physical resources. That is only
     *        correct when the caller has already waited the device idle.
     *        A defaulted parameter would make the unsafe path the one you get by
     *        forgetting, so the choice is forced to the call site: pass a callback
     *        that routes into a fence-gated retirement queue (what
     *        RenderScene::retireViewResourceState does), or pass nullptr and own
     *        the fact that you waited.
     * @param update_mask   External resource group mask (default: GROUP_SWAPCHAIN)
     * @return true on success
     */
    LUX_FUNCTION_PUBLIC bool ResizeViewResources(
        const RGCompiledGraph& graph,
        VkExtent2D new_extent,
        RGResourceState& state,
        RGVulkanRecorder& recorder,
        PhysicalResourceAllocator& allocator,
        uint32_t frames_in_flight,
        std::function<void(RGPhysicalResourceTable&)> on_resources_allocated,
        std::function<void(RGResourceState&&)> on_old_state_retired,
        uint32_t update_mask = static_cast<uint32_t>(RGUpdateGroup::GROUP_SWAPCHAIN)
    );

} // namespace lux::render
