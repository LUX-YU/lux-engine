#pragma once

#include <NRI.h>
#include <lux/engine/render/device/DeviceId.hpp>
#include <lux/engine/render/runtime/JobId.hpp>
#include <lux/engine/render/effect/EffectId.hpp>
#include <optional>
#include <lux/engine/render/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/runtime/Target.hpp>
#include <lux/engine/render/runtime/TargetId.hpp>
#include <span>

namespace lux::render::detail
{
    // Control-thread state attached to either physical Target alternative.
    struct TargetState final
    {
        DeviceSessionId device;
        std::uint64_t epoch = 1;
        RenderJobId writer;
        bool enabled = true;
        // Engaged while Resize or Remove owns the Target. A supplied Effect stays live until adoption or rollback.
        std::optional<EffectId> pending_replacement;

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
        SubmissionFrontiers* accepted_uses = nullptr; // Includes producer and external readers.
        SubmissionFrontiers* producer_uses = nullptr; // Published image readers wait for the producer only.
    };

    // Physical output borrow. GPU frame slot and swap-chain image index remain distinct:
    // a SwapchainTarget assigns the acquired image to the selected reusable frame slot.
    struct TargetOutput final
    {
        TargetDesc description;
        std::span<const TargetFrame> frames;
    };
} // namespace lux::render::detail
