#include <limits>
#include <lux/engine/render/runtime/FrameProgress.hpp>

namespace lux::render::detail
{
    FrameProgress::FrameProgress(std::uint32_t count, const void* domain, std::size_t queues) noexcept
    {
        const bool is_invalid_configuration = count == 0 || queues == 0 || !domain;
        if (is_invalid_configuration)
        {
            std::terminate();
        }
        slots_.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            slots_.emplace_back(domain, queues);
        }
    }

    FrameProgress::~FrameProgress() noexcept
    {
        if (active_)
        {
            std::terminate();
        }
    }

    RenderResult<bool> FrameProgress::tryBegin(const SubmissionFrontiers& completed, Round& round) noexcept
    {
        const bool is_invalid_round = round.active() || !slots_.front().compatible(completed);
        if (is_invalid_round)
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        if (active_)
        {
            return false;
        }
        if (next_frame_index_ == std::numeric_limits<std::uint64_t>::max())
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        const auto slot = static_cast<std::uint32_t>(next_frame_index_ % slots_.size());
        if (!slots_[slot].reachedBy(completed))
        {
            return false;
        }
        slots_[slot].clear();
        round.owner_ = this;
        round.frame_ = {next_frame_index_++, slot};
        active_ = &round;
        return true;
    }

    FrameProgress::Round::~Round() noexcept
    {
        finish();
    }

    FrameContext FrameProgress::Round::context() const noexcept
    {
        if (!owner_)
        {
            std::terminate();
        }
        return frame_;
    }

    void FrameProgress::Round::accept(SubmissionPoint accepted) noexcept
    {
        const bool is_invalid_round = !owner_ || owner_->active_ != this;
        if (is_invalid_round)
        {
            std::terminate();
        }
        owner_->slots_[frame_.slot_index].accept(accepted);
    }

    void FrameProgress::Round::finish() noexcept
    {
        if (!owner_)
        {
            return;
        }
        if (owner_->active_ != this)
        {
            std::terminate();
        }
        owner_->active_ = nullptr;
        owner_ = nullptr;
    }
} // namespace lux::render::detail
