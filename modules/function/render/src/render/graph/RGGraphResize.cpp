#include <lux/engine/render/graph/RGGraphResize.hpp>
#include <lux/engine/render/graph/RGVulkanRecorder.hpp>
#include <lux/engine/render/graph/PhysicalResourceAllocator.hpp>
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGResourceTypes.hpp>

#include <utility>

namespace lux::render
{
    bool ResizeViewResources(
        const RGCompiledGraph&          graph,
        VkExtent2D                      new_extent,
        RGResourceState&                state,
        RGVulkanRecorder&               recorder,
        PhysicalResourceAllocator&      allocator,
        uint32_t                        frames_in_flight,
        std::function<void(RGPhysicalResourceTable&)> on_resources_allocated,
        std::function<void(RGResourceState&&)>        on_old_state_retired,
        uint32_t                        update_mask)
    {
        // 1. Check if resize is actually needed
        if (state.current_extent.width == new_extent.width &&
            state.current_extent.height == new_extent.height)
        {
            return true; // No resize needed
        }

        // 2. Detach old state; retire/deallocate only after new state is ready.
        RGResourceState old_state{};
        const bool had_old_state = (state.record_ctx.frames_in_flight > 0);
        if (had_old_state)
        {
            old_state = std::move(state);
            state = {};
        }

        // 3. Reallocate physical resources at the new extent.
        //    The allocator resolves RELATIVE_MODE sizes internally — no graph mutation needed.
        RGCompileOptions options{};
        auto alloc_result = allocator.allocate(
            graph.original_graph,
            graph.dependency_info,
            options,
            new_extent,
            frames_in_flight
        );
        if (!alloc_result)
        {
            if (had_old_state)
                state = std::move(old_state);
            return false;
        }
        RGPhysicalResourceTable new_physical_resources = std::move(*alloc_result);

        // 4. Refresh external resources (e.g. swapchain images)
        allocator.refreshExternalResources(
            new_physical_resources,
            graph.dynamic_external_resources,
            update_mask
        );

        // 5. Optional callback for additional manual injection
        if (on_resources_allocated)
        {
            on_resources_allocated(new_physical_resources);
        }

        // 6. Recreate record context (image views, group extents, etc.)
        auto result = recorder.allocateRecordContext(graph, new_physical_resources, new_extent, frames_in_flight);
        if (!result)
        {
            // Roll back allocation made for the new extent. deallocateToPool is
            // virtual: pooling allocators reuse, others fall back to deallocate().
            allocator.deallocateToPool(new_physical_resources, graph.original_graph);

            if (had_old_state)
                state = std::move(old_state);
            return false;
        }

        // 7. Commit new state
        state.current_extent = new_extent;
        state.physical_resources = std::move(new_physical_resources);
        state.record_ctx = std::move(result.value());

        // 8. Retire old state after successful swap
        if (had_old_state)
        {
            if (on_old_state_retired)
            {
                on_old_state_retired(std::move(old_state));
            }
            else
            {
                recorder.deallocateRecordContext(old_state.record_ctx);
                allocator.deallocateToPool(old_state.physical_resources, graph.original_graph);
            }
        }

        return true;
    }

} // namespace lux::render
