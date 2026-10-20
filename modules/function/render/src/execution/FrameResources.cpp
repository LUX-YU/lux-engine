#include <lux/engine/render/execution/FrameResources.hpp>

namespace lux::render::detail
{
    void ExecutionContext::FrameResources::release(const nri::CoreInterface& nri_core) noexcept
    {
        nri_core.DestroyDescriptorPool(descriptor_pool);
        for (auto& binding : bindings)
        {
            nri_core.DestroyDescriptor(binding.buffer_view);
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
    }
} // namespace lux::render::detail
