#include <lux/engine/render/execution/FrameResources.hpp>

namespace lux::render::detail
{
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
        for (const auto& allocation : memories)
        {
            nri_core.FreeMemory(allocation.memory);
        }
    }
} // namespace lux::render::detail
