#pragma once
/**
 * @file FrameExternalSync.hpp
 * @brief Public entry for injecting external (e.g. CUDA-produced) timeline
 *        semaphores into the current frame's GRAPHICS submit.
 *
 * A feature that imports an externally-produced image (via RGBuilder::importTexture)
 * must make the engine's graphics queue WAIT until the producer finished writing it,
 * and (for ping-pong) SIGNAL when the engine is done so the producer's next frame can
 * proceed without a host stall. These two calls express exactly that, from inside
 * RenderFeature::populateFrameContext(RGFrameContext&).
 *
 * RGFrameContext is engine-internal (its full layout lives in a non-public header), so
 * a feature only forward-declares it (it already receives one by reference). These free
 * functions are the narrow, public seam that writes the external-sync fields without
 * exposing the struct. The header traffics only a forward-declared Vulkan HANDLE
 * (core/vk_fwd.hpp) + uint64_t, so it pulls NO <vulkan/vulkan.h> — same tier policy as
 * the rest of the public base surface.
 */

#include <lux/engine/render/core/vk_fwd.hpp> // VkSemaphore (handle fwd-decl, no vulkan.h)
#include <lux/engine/function/visibility.h>

#include <cstdint>

namespace lux::render
{
    struct RGFrameContext; // engine-internal; defined in graph/RGRecorder.hpp

    /// Make this frame's GRAPHICS submit WAIT until @p semaphore reaches @p value
    /// before executing — so it samples the producer's finished output. @p stage is
    /// a VkPipelineStageFlags2 (e.g. VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT), passed
    /// as uint64_t to keep this header vulkan.h-free. Call from populateFrameContext.
    LUX_FUNCTION_PUBLIC void
    addExternalGraphicsWait(RGFrameContext& ctx, VkSemaphore semaphore, uint64_t value, uint64_t stage);

    /// Make this frame's GRAPHICS submit SIGNAL @p semaphore to @p value when done,
    /// so the producer's next frame can wait on it (ping-pong). NOTE: a signal applies
    /// to ONE submit; with multiple views/submits per frame, inject the signal from a
    /// single view only (signaling the same timeline value twice is invalid).
    LUX_FUNCTION_PUBLIC void
    addExternalGraphicsSignal(RGFrameContext& ctx, VkSemaphore semaphore, uint64_t value, uint64_t stage);
} // namespace lux::render
