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
        Accepted,
        ValidatedAndReserved,
        TransferQueued,
        RecordedOrTransferComplete,
        GraphicsFinalizeSubmitted,
        Ready,
        Failed,
    };

    [[nodiscard]] constexpr bool isUploadLifecycleTerminal(EUploadLifecycleState state) noexcept
    {
        return state == EUploadLifecycleState::Ready || state == EUploadLifecycleState::Failed;
    }

    /// Legal edges of the render-owner state machine. Failure is terminal
    /// from every live state; success is legal only after the low-level copy
    /// has completed, optionally followed by graphics-queue finalization.
    [[nodiscard]] constexpr bool
    isValidUploadLifecycleTransition(EUploadLifecycleState from, EUploadLifecycleState to) noexcept
    {
        if (isUploadLifecycleTerminal(from))
            return false;
        if (to == EUploadLifecycleState::Failed)
            return true;

        switch (from)
        {
        case EUploadLifecycleState::Accepted:
            return to == EUploadLifecycleState::ValidatedAndReserved;
        case EUploadLifecycleState::ValidatedAndReserved:
            return to == EUploadLifecycleState::TransferQueued;
        case EUploadLifecycleState::TransferQueued:
            return to == EUploadLifecycleState::RecordedOrTransferComplete;
        case EUploadLifecycleState::RecordedOrTransferComplete:
            return to == EUploadLifecycleState::GraphicsFinalizeSubmitted || to == EUploadLifecycleState::Ready;
        case EUploadLifecycleState::GraphicsFinalizeSubmitted:
            return to == EUploadLifecycleState::Ready;
        case EUploadLifecycleState::Ready:
        case EUploadLifecycleState::Failed:
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
