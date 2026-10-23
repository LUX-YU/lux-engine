#pragma once

#include <NRI.h>
#include <lux/engine/render/core/Target.hpp>
#include <span>

namespace lux::render::detail
{
    // One acquired presentation image. Submission records consumption/signalling only after
    // QueueSubmit accepts the work; SurfaceTarget owns both semaphores and performs presentation.
    struct PresentationSync final
    {
        nri::Fence* acquired = nullptr;
        nri::Fence* rendered = nullptr;
        bool acquired_consumed = false;
        bool rendered_signalled = false;
    };

    struct TargetFrame final
    {
        nri::Texture* image = nullptr;
        nri::Descriptor* view = nullptr;
        PresentationSync* presentation = nullptr;
    };

    // Physical output borrow. GPU frame slot and swap-chain image index remain distinct:
    // a SurfaceTarget assigns the acquired image to the selected reusable frame slot.
    struct TargetOutput final
    {
        TargetDesc description;
        std::span<const TargetFrame> frames;
    };
} // namespace lux::render::detail
