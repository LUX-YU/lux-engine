#include <exception>
#include <lux/engine/render/target/SwapchainFlight.hpp>

namespace lux::render::detail
{
    SwapchainFlight::SwapchainFlight(std::uint32_t image_count) noexcept : image_count_(image_count)
    {
        if (image_count == 0) std::terminate();
    }

    SwapchainFlight::~SwapchainFlight() noexcept
    {
        const bool is_unsettled = operation_ || state_ != EState::RETIRED;
        if (is_unsettled)
        {
            std::terminate();
        }
    }

    SwapchainFlight::Operation::~Operation() noexcept
    {
        if (owner_)
        {
            std::terminate();
        }
    }

    bool SwapchainFlight::Operation::is(EState state) const noexcept
    {
        return owner_ && owner_->operation_ == this && owner_->state_ == state;
    }

    void SwapchainFlight::Operation::finish(EState state) noexcept
    {
        owner_->state_ = state;
        owner_->operation_ = nullptr;
        owner_ = nullptr;
    }

    void SwapchainFlight::start(Operation& operation, EState state) noexcept
    {
        operation_ = &operation;
        operation.owner_ = this;
        state_ = state;
    }

    RenderResult<bool> SwapchainFlight::beginAcquire(Operation& operation) noexcept
    {
        if (operation.active())
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        if (state_ != EState::IDLE)
        {
            return false;
        }
        start(operation, EState::ACQUIRING);
        return true;
    }

    RenderResult<void> SwapchainFlight::beginSubmit(Operation& operation) noexcept
    {
        const bool is_invalid_entry = operation.active() || state_ != EState::READY;
        if (is_invalid_entry)
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        start(operation, EState::SUBMITTING);
        return {};
    }

    RenderResult<void> SwapchainFlight::beginPresent(Operation& operation) noexcept
    {
        const bool is_invalid_entry = operation.active() || state_ != EState::SUBMITTED;
        if (is_invalid_entry)
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        start(operation, EState::PRESENTING);
        return {};
    }

    RenderResult<void> SwapchainFlight::beginRetire(Operation& operation) noexcept
    {
        const bool has_native_borrow = operation.active() || operation_;
        if (has_native_borrow)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        if (state_ == EState::RETIRED)
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        start(operation, EState::RETIRING);
        return {};
    }

    RenderResult<void> SwapchainFlight::Operation::acquired(RenderResult<AcquiredImage> result) noexcept
    {
        if (!is(EState::ACQUIRING))
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        if (!result)
        {
            finish(EState::FAILED);
            return cxx::unexpected(result.error());
        }
        switch (result->status)
        {
        case EAcquireStatus::ACQUIRED:
            if (result->image_index >= owner_->image_count_)
            {
                break;
            }
            owner_->image_index_ = result->image_index;
            finish(EState::READY);
            return {};
        case EAcquireStatus::NOT_READY:
            finish(EState::IDLE);
            return {};
        case EAcquireStatus::RECREATE_REQUIRED:
            finish(EState::RECREATE);
            return {};
        }
        finish(EState::FAILED);
        return cxx::unexpected(RenderError{kFrameContract});
    }

    RenderResult<void> SwapchainFlight::Operation::submitted(RenderResult<void> result) noexcept
    {
        if (!is(EState::SUBMITTING))
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        finish(result ? EState::SUBMITTED : EState::FAILED);
        return result;
    }

    RenderResult<void> SwapchainFlight::Operation::presented(RenderResult<EPresentStatus> result) noexcept
    {
        if (!is(EState::PRESENTING))
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        if (!result)
        {
            finish(EState::FAILED);
            return cxx::unexpected(result.error());
        }
        switch (*result)
        {
        case EPresentStatus::QUEUED:
            owner_->image_index_ = UINT32_MAX;
            finish(EState::IDLE); // Present acceptance is NOT WSI completion or semaphore reuse proof.
            return {};
        case EPresentStatus::NOT_READY:
            finish(EState::SUBMITTED); // Not enqueued; preserve image and signal for a later attempt.
            return {};
        case EPresentStatus::RECREATE_REQUIRED:
            finish(EState::RECREATE);
            return {};
        }
        finish(EState::FAILED);
        return cxx::unexpected(RenderError{kFrameContract});
    }

    RenderResult<void> SwapchainFlight::Operation::retired(RenderResult<void> result) noexcept
    {
        if (!is(EState::RETIRING))
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        if (result)
        {
            owner_->image_index_ = UINT32_MAX;
        }
        finish(result ? EState::RETIRED : EState::FAILED);
        return result;
    }
} // namespace lux::render::detail
