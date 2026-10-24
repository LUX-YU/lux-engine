#include <atomic>
#include <lux/engine/render/execution/FrameResources.hpp>

namespace lux::render::detail
{
    void RenderGraphInstance::FrameResources::invalidateImportedBindings() noexcept
    {
        // Unique cache revisions prevent a reused instance address from matching an older binding cache.
        static std::atomic<std::uint64_t> next_revision{1};
        imported_revision = next_revision.fetch_add(1, std::memory_order_relaxed);
    }

    void RenderGraphInstance::FrameResources::release(const nri::CoreInterface& nri_core) noexcept
    {
        nri_core.DestroyDescriptorPool(descriptor_pool);
        for (auto& pass : passes)
        {
            for (auto* view : pass.private_views)
            {
                nri_core.DestroyDescriptor(view);
            }
        }
        for (auto* view : views)
        {
            nri_core.DestroyDescriptor(view);
        }
        for (auto* image : images)
        {
            nri_core.DestroyTexture(image);
        }
        for (auto* buffer : buffers)
        {
            nri_core.DestroyBuffer(buffer);
        }
        for (auto& list : draw_lists)
        {
            nri_core.DestroyBuffer(list.vertices);
            nri_core.DestroyBuffer(list.indices);
        }
        for (const auto& allocation : memories)
        {
            nri_core.FreeMemory(allocation.memory);
        }
    }
} // namespace lux::render::detail
