#pragma once

#include <lux/engine/render/commands/Error.hpp>
#include <optional>

namespace lux::render::detail
{
    enum class EAcquireStatus : std::uint8_t
    {
        ACQUIRED,
        NOT_READY,
        RECREATE_REQUIRED
    };

    struct AcquiredImage final
    {
        EAcquireStatus status;
        std::uint32_t image_index = UINT32_MAX;
    };
    enum class EPresentStatus : std::uint8_t
    {
        QUEUED,
        NOT_READY,
        RECREATE_REQUIRED
    };

    // One Target's Acquire/Submit/Present association, independent of Job and frame-slot lifetime.
    // Construct with a live swapchain; only actual backend retirement authorizes its destruction.
    class SwapchainFlight final
    {
    public:
        enum class EState : std::uint8_t
        {
            IDLE,
            ACQUIRING,
            READY,
            SUBMITTING,
            SUBMITTED,
            PRESENTING,
            RECREATE,
            FAILED,
            RETIRING,
            RETIRED
        };

        // Embed in the actual connected native operation, which retains Target and result storage
        // through Server delivery. This node is a borrow, not a sender, queue or native owner.
        class Operation final
        {
        public:
            Operation() noexcept = default;
            ~Operation() noexcept;
            Operation(const Operation&) = delete;
            Operation& operator=(const Operation&) = delete;
            [[nodiscard]] RenderResult<void> acquired(RenderResult<AcquiredImage> result) noexcept;
            [[nodiscard]] RenderResult<void> submitted(RenderResult<void> result) noexcept;
            [[nodiscard]] RenderResult<void> presented(RenderResult<EPresentStatus> result) noexcept;
            [[nodiscard]] RenderResult<void> retired(RenderResult<void> result) noexcept;

            [[nodiscard]] bool active() const noexcept
            {
                return owner_ != nullptr;
            }

        private:
            friend class SwapchainFlight;
            [[nodiscard]] bool is(EState state) const noexcept;
            void finish(EState state) noexcept;
            SwapchainFlight* owner_ = nullptr;
        };

        explicit SwapchainFlight(std::uint32_t image_count) noexcept;
        ~SwapchainFlight() noexcept;
        SwapchainFlight(const SwapchainFlight&) = delete;
        SwapchainFlight& operator=(const SwapchainFlight&) = delete;
        [[nodiscard]] RenderResult<bool> beginAcquire(Operation& operation) noexcept;
        [[nodiscard]] RenderResult<void> beginSubmit(Operation& operation) noexcept;
        [[nodiscard]] RenderResult<void> beginPresent(Operation& operation) noexcept;
        [[nodiscard]] RenderResult<void> beginRetire(Operation& operation) noexcept;

        [[nodiscard]] EState state() const noexcept
        {
            return state_;
        }

        [[nodiscard]] bool ready() const noexcept
        {
            return state_ == EState::READY;
        }

        [[nodiscard]] bool borrowed() const noexcept
        {
            return operation_ != nullptr;
        }

        [[nodiscard]] bool canDestroy() const noexcept
        {
            return state_ == EState::RETIRED;
        }

        [[nodiscard]] std::optional<std::uint32_t> imageIndex() const noexcept
        {
            return image_index_ == UINT32_MAX ? std::nullopt : std::optional{image_index_};
        }

    private:
        void start(Operation& operation, EState state) noexcept;
        std::uint32_t image_count_;
        EState state_ = EState::IDLE;
        std::uint32_t image_index_ = UINT32_MAX;
        Operation* operation_ = nullptr;
    };
} // namespace lux::render::detail
