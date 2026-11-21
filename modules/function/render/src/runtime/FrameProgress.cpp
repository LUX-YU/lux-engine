#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/runtime/FrameProgress.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

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
        if (access_ != ECpuAccess::IDLE)
        {
            std::terminate();
        }
    }

    std::uint32_t FrameProgress::nextSlot() const noexcept
    {
        return static_cast<std::uint32_t>(next_frame_index_ % slots_.size());
    }

    FrameProgress::SlotSelection FrameProgress::beginCpuAccess(ECpuAccess access) noexcept
    {
        if (access_ != ECpuAccess::IDLE || epoch_ == UINT64_MAX) std::terminate();
        access_slot_ = nextSlot();
        access_ = access;
        return {{next_frame_index_, access_slot_}, ++epoch_};
    }

    void FrameProgress::requireAccess(ReplicaFrame selection, ECpuAccess access) const noexcept
    {
        const bool wrong_operation = selection.epoch != epoch_ || selection.slot != access_slot_;
        if (access_ != access || wrong_operation) std::terminate();
    }

    void FrameProgress::endCpuAccess(ReplicaFrame selection, ECpuAccess access) noexcept
    {
        requireAccess(selection, access);
        access_ = ECpuAccess::IDLE;
    }

    RenderResult<std::optional<FrameProgress::SlotSelection>>
    FrameProgress::tryBegin(const DeviceSession& device) noexcept
    {
        const bool exhausted = epoch_ == UINT64_MAX || next_frame_index_ == UINT64_MAX;
        if (exhausted) return cxx::unexpected(RenderError{kFrameContract});
        device.readCompleted(completed_);
        if (!canBegin(completed_)) return std::nullopt;
        const auto selection = beginCpuAccess(ECpuAccess::FRAME);
        slots_[selection.frame.slot_index].clear();
        ++next_frame_index_;
        return selection;
    }

    void FrameProgress::adopt(SlotSelection selection, SubmissionPoint accepted) noexcept
    {
        requireAccess({selection.frame.slot_index, selection.epoch}, ECpuAccess::FRAME);
        slots_[selection.frame.slot_index].accept(accepted);
    }

    void FrameProgress::endFrame(SlotSelection selection) noexcept
    {
        endCpuAccess({selection.frame.slot_index, selection.epoch}, ECpuAccess::FRAME);
    }

    bool FrameProgress::canBegin(const SubmissionFrontiers& completed) const noexcept
    {
        return access_ == ECpuAccess::IDLE && slots_[nextSlot()].reachedBy(completed);
    }

    bool FrameProgress::canBegin(const DeviceSession& device) noexcept
    {
        device.readCompleted(completed_);
        return canBegin(completed_);
    }

    exec::task<RenderResult<ReplicaFrame>> FrameProgress::acquireReplica(DeviceSession& device) noexcept
    {
        if (access_ != ECpuAccess::IDLE) co_return cxx::unexpected(RenderError{kDeviceBusy});
        if (epoch_ == UINT64_MAX) co_return cxx::unexpected(RenderError{kFrameContract});
        const auto selected = beginCpuAccess(ECpuAccess::REPLICA);
        const ReplicaFrame selection{selected.frame.slot_index, selected.epoch};
        cxx::scope_exit cancel([&]() noexcept { endReplica(selection); });
        auto ready = co_await device.waitFrontiers(slots_[selection.slot]);
        if (!ready) co_return cxx::unexpected(ready.error());
        cancel.release();
        co_return selection;
    }

    SubmissionFrontiers& FrameProgress::replicaFrontiers(ReplicaFrame selection) noexcept
    {
        requireAccess(selection, ECpuAccess::REPLICA);
        return slots_[selection.slot];
    }

    void FrameProgress::endReplica(ReplicaFrame selection) noexcept
    {
        endCpuAccess(selection, ECpuAccess::REPLICA);
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

namespace lux::render
{
    ReplicaMutation::~ReplicaMutation() noexcept
    {
        if (selected_.epoch) device_.frameProgress().endReplica(selected_);
    }

    exec::task<RenderResult<detail::ReplicaFrame>> ReplicaMutation::select() noexcept
    {
        if (!device_.uploader()) co_return cxx::unexpected(RenderError{kDeviceUnsupported});
        if (!selected_.epoch)
        {
            auto selected = co_await device_.frameProgress().acquireReplica(device_.session());
            if (!selected) co_return cxx::unexpected(selected.error());
            selected_ = *selected;
        }
        co_return selected_;
    }

    exec::task<RenderResult<void>> ReplicaMutation::writeBuffers(std::span<const detail::BufferWrite> writes) noexcept
    {
        if (!selected_.epoch) co_return cxx::unexpected(RenderError{kFrameContract});
        auto& frontiers = device_.frameProgress().replicaFrontiers(selected_);
        co_return co_await device_.uploader()->writeReplicaBuffers(writes, frontiers);
    }
}
