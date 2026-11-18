#include <limits>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/runtime/FrameProgress.hpp>

namespace lux::render::detail
{
    FrameProgress::FrameProgress(std::uint32_t count, const void* domain, std::size_t queues) noexcept
        : completed_(domain, queues)
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

    RenderResult<bool> FrameProgress::tryBegin(const DeviceSession& device, Round& round) noexcept
    {
        device.readCompleted(completed_);
        return tryBegin(completed_, round);
    }

    FrameProgress::~FrameProgress() noexcept
    {
        if (active_ || writing_)
        {
            std::terminate();
        }
    }

    std::uint32_t FrameProgress::nextSlot() const noexcept
    {
        return static_cast<std::uint32_t>(next_frame_index_ % slots_.size());
    }

    void FrameProgress::advance() noexcept
    {
        if (next_frame_index_ == std::numeric_limits<std::uint64_t>::max())
        {
            std::terminate();
        }
        ++next_frame_index_;
    }

    RenderResult<bool> FrameProgress::tryBegin(const SubmissionFrontiers& completed, Round& round) noexcept
    {
        const bool is_invalid_round = round.active() || !slots_.front().compatible(completed);
        if (is_invalid_round)
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        if (next_frame_index_ == std::numeric_limits<std::uint64_t>::max())
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        const auto slot = nextSlot();
        if (!canBegin(completed))
        {
            return false;
        }
        slots_[slot].clear();
        round.owner_ = this;
        round.frame_ = {next_frame_index_, slot};
        advance();
        active_ = &round;
        return true;
    }

    bool FrameProgress::canBegin(const SubmissionFrontiers& completed) const noexcept
    {
        return !active_ && !writing_ && slots_[nextSlot()].reachedBy(completed);
    }

    bool FrameProgress::canBegin(const DeviceSession& device) noexcept
    {
        device.readCompleted(completed_);
        return canBegin(completed_);
    }

    exec::task<RenderResult<void>> FrameProgress::acquireReplica(DeviceSession& device, ReplicaWrite& write) noexcept
    {
        if (active_ || writing_ || write.owner_)
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        write.slot_ = nextSlot();
        write.accepted_ = device.makeFrontiers();
        write.owner_ = this;
        writing_ = &write;
        co_return co_await device.waitFrontiers(slots_[write.slot_]);
    }

    FrameProgress::ReplicaWrite::~ReplicaWrite() noexcept
    {
        if (!owner_) return;
        if (owner_->writing_ != this) std::terminate();
        owner_->slots_[slot_].merge(accepted_);
        owner_->writing_ = nullptr;
    }

    RenderResult<bool> FrameProgress::waitForProgress(DeviceSession& device, std::uint64_t timeout_ns) noexcept
    {
        const auto count = device.queueCount();
        const auto frontier = slots_[nextSlot()].serials();
        for (std::size_t offset = 0; offset < count; ++offset)
        {
            const auto index = (wait_queue_cursor_ + offset) % count;
            auto& queue = device.physicalQueue(index);
            const auto serial = frontier[index];
            if (serial > queue.completed())
            {
                wait_queue_cursor_ = (index + 1) % count;
                return queue.wait(serial, timeout_ns);
            }
        }
        for (std::size_t offset = 0; offset < count; ++offset)
        {
            const auto index = (wait_queue_cursor_ + offset) % count;
            auto& queue = device.physicalQueue(index);
            if (queue.submitted() > queue.completed())
            {
                wait_queue_cursor_ = (index + 1) % count;
                return queue.wait(queue.completed() + 1, timeout_ns);
            }
        }
        return false;
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

    const SubmissionFrontiers& FrameProgress::Round::accepted() const noexcept
    {
        if (!owner_) std::terminate();
        return owner_->frontiers(frame_.slot_index);
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
