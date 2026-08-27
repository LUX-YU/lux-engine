#include <lux/engine/render/graph/FrameExternalSync.hpp>

// Here RGFrameContext is a complete type, so the public free functions can write its
// external-sync vectors. The vulkan.h pulled here is the .cpp's own business — the
// public header (FrameExternalSync.hpp) stays handle-only / vulkan.h-free.
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <vulkan/vulkan.h>

namespace lux::render
{
    namespace
    {
        VkSemaphoreSubmitInfo makeSemaphoreSubmit(VkSemaphore sem, uint64_t value, uint64_t stage)
        {
            VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            info.semaphore = sem;
            info.value = value;
            info.stageMask = static_cast<VkPipelineStageFlags2>(stage);
            return info;
        }
    } // namespace

    void addExternalGraphicsWait(RGFrameContext& ctx, VkSemaphore semaphore, uint64_t value, uint64_t stage)
    {
        ctx.external_graphics_waits.push_back(makeSemaphoreSubmit(semaphore, value, stage));
    }

    void addExternalGraphicsSignal(RGFrameContext& ctx, VkSemaphore semaphore, uint64_t value, uint64_t stage)
    {
        ctx.external_graphics_signals.push_back(makeSemaphoreSubmit(semaphore, value, stage));
    }
} // namespace lux::render
