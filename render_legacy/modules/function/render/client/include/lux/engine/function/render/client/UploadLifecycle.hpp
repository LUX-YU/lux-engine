#pragma once

#include <cstddef>
#include <cstdint>

namespace lux::render
{
    /// Render-thread-owned lifecycle for a low-level persistent GPU upload.
    /// Values are ordered; transitions may skip GraphicsFinalizeSubmitted when
    /// no graphics-queue acquire/mip/copy submit is required.
    enum class EUploadLifecycleState : std::uint8_t
    {
        ACCEPTED,
        VALIDATED_AND_RESERVED,
        TRANSFER_QUEUED,
        RECORDED_OR_TRANSFER_COMPLETE,
        GRAPHICS_FINALIZE_SUBMITTED,
        READY,
        FAILED,
    };

    [[nodiscard]] constexpr bool isUploadLifecycleTerminal(EUploadLifecycleState state) noexcept
    {
        return state == EUploadLifecycleState::READY || state == EUploadLifecycleState::FAILED;
    }

    /// Legal edges of the render-owner state machine. Failure is terminal
    /// from every live state; success is legal only after the low-level copy
    /// has completed, optionally followed by graphics-queue finalization.
    [[nodiscard]] constexpr bool isValidUploadLifecycleTransition(
        EUploadLifecycleState from,
        EUploadLifecycleState to
    ) noexcept
    {
        if (isUploadLifecycleTerminal(from))
            return false;
        if (to == EUploadLifecycleState::FAILED)
            return true;

        switch (from)
        {
        case EUploadLifecycleState::ACCEPTED:
            return to == EUploadLifecycleState::VALIDATED_AND_RESERVED;
        case EUploadLifecycleState::VALIDATED_AND_RESERVED:
            return to == EUploadLifecycleState::TRANSFER_QUEUED;
        case EUploadLifecycleState::TRANSFER_QUEUED:
            return to == EUploadLifecycleState::RECORDED_OR_TRANSFER_COMPLETE;
        case EUploadLifecycleState::RECORDED_OR_TRANSFER_COMPLETE:
            return to == EUploadLifecycleState::GRAPHICS_FINALIZE_SUBMITTED || to == EUploadLifecycleState::READY;
        case EUploadLifecycleState::GRAPHICS_FINALIZE_SUBMITTED:
            return to == EUploadLifecycleState::READY;
        case EUploadLifecycleState::READY:
        case EUploadLifecycleState::FAILED:
            return false;
        }
        return false;
    }

    struct UploadLifecycleSnapshot
    {
        std::uint64_t accepted{0};
        std::uint64_t terminal_ready{0};
        std::uint64_t terminal_failed{0};
        std::uint64_t stale_result{0};
        std::uint64_t duplicate_terminal{0};
        /// Bytes copied from immutable CPU owners into Vulkan staging memory.
        /// This is the necessary final CPU copy, distinct from producer-side
        /// payload cloning reported by RenderUploadClientStatistics.
        std::uint64_t staging_copied_bytes{0};
        std::size_t active{0};

        [[nodiscard]] bool clean() const noexcept
        {
            return active == 0 && accepted == terminal_ready + terminal_failed;
        }
    };
} // namespace lux::render
