#pragma once

#include <NRI.h>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <span>

namespace lux::render::detail
{
    // Control-thread state attached to either physical Target alternative.
    struct TargetState final
    {
        std::uint64_t epoch = 1;
        RenderJobId writer;
        const void* reservation = nullptr; // The connected Job start/rebind owns this reservation.
        bool enabled = true;
        bool resizing = false;

        void releaseWriter(RenderJobId id) noexcept
        {
            if (writer == id) writer = {};
        }
    };

    enum class EPresentationPhase : std::uint8_t
    {
        IDLE,
        ACQUIRING,
        READY,
        WAIT_ACCEPTED,
        SUBMITTED,
        PRESENTING,
        FAILED
    };

    // Shared with submission for the two actual queue receipts. The Target owns the flight.
    struct PresentationSync final
    {
        EPresentationPhase phase = EPresentationPhase::IDLE;
        nri::Fence* acquired = nullptr;
        nri::Fence* rendered = nullptr;
        SubmissionPoint* acquire_use = nullptr;
    };

    struct TargetFrame final
    {
        nri::Texture* image = nullptr;
        nri::Descriptor* view = nullptr;
        PresentationSync* presentation = nullptr;
        std::uint32_t sampled_index = UINT32_MAX;
        SubmissionFrontiers* accepted_uses = nullptr; // Output borrow recorded at native acceptance.
    };

    // Physical output borrow. GPU frame slot and swap-chain image index remain distinct:
    // a SwapchainTarget assigns the acquired image to the selected reusable frame slot.
    struct TargetOutput final
    {
        TargetDesc description;
        std::span<const TargetFrame> frames;
    };
} // namespace lux::render::detail
