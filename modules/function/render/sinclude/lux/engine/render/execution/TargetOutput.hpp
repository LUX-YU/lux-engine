#pragma once

#include <NRI.h>
#include <lux/engine/render/core/Target.hpp>
#include <span>

namespace lux::render::detail
{
    // One acquired presentation image. Flags record accepted submissions, not GPU completion.
    // Submission records the binary wait/signal only after
    // QueueSubmit accepts the work; SurfaceTarget owns both semaphores and performs presentation.
    struct PresentationSync final
    {
        nri::Fence* acquired = nullptr;
        nri::Fence* rendered = nullptr;
        bool acquire_wait_submitted = false;
        bool render_signal_submitted = false;
    };

    struct TargetFrame final
    {
        nri::Texture* image = nullptr;
        nri::Descriptor* view = nullptr;
        PresentationSync* presentation = nullptr;
        std::uint32_t sampled_index = UINT32_MAX;
    };

    // Physical output borrow. GPU frame slot and swap-chain image index remain distinct:
    // a SurfaceTarget assigns the acquired image to the selected reusable frame slot.
    struct TargetOutput final
    {
        TargetDesc description;
        std::span<const TargetFrame> frames;
    };
} // namespace lux::render::detail
