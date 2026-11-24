#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
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

    FrameProgress::~FrameProgress() noexcept
    {
        if (active_)
        {
            std::terminate();
        }
    }

    std::uint32_t FrameProgress::nextSlot() const noexcept
    {
        return static_cast<std::uint32_t>(next_frame_index_ % slots_.size());
    }

    void FrameProgress::requireFrame(FrameContext selected) const noexcept
    {
        const bool current = active_ && selected.frame_index == next_frame_index_ - 1 &&
            selected.slot_index == selected.frame_index % slots_.size();
        if (!current) std::terminate();
    }

    RenderResult<std::optional<FrameContext>>
    FrameProgress::availableFrame(const DeviceSession& device) noexcept
    {
        if (next_frame_index_ == UINT64_MAX) return cxx::unexpected(RenderError{kFrameContract});
        device.readCompleted(completed_);
        if (active_ || !slots_[nextSlot()].reachedBy(completed_)) return std::nullopt;
        return FrameContext{next_frame_index_, nextSlot()};
    }

    void FrameProgress::beginFrame(FrameContext selection) noexcept
    {
        const bool is_current = selection.frame_index == next_frame_index_ && selection.slot_index == nextSlot();
        if (active_ || !is_current || !slots_[selection.slot_index].reachedBy(completed_)) std::terminate();
        active_ = true;
        slots_[selection.slot_index].clear();
        ++next_frame_index_;
    }

    void FrameProgress::adopt(FrameContext selection, SubmissionPoint accepted) noexcept
    {
        requireFrame(selection);
        slots_[selection.slot_index].accept(accepted);
    }

    void FrameProgress::endFrame(FrameContext selection) noexcept
    {
        requireFrame(selection);
        active_ = false;
    }

    RenderResult<bool> FrameProgress::waitForProgress(DeviceSession& device, std::uint64_t timeout_ns) noexcept
    {
        const auto count = device.queueCount();
        const auto& frontier = slots_[nextSlot()].serials();
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

} // namespace lux::render::detail
