#pragma once

#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/visibility.h>
#include <exec/task.hpp>
#include <optional>
#include <span>

namespace lux::render::detail
{
    class DeviceSession;

    struct FrameContext final
    {
        std::uint64_t frame_index = 0;
        std::uint32_t slot_index = 0;
        bool operator==(const FrameContext&) const noexcept = default;
    };

    // DeviceEntry's strict round progress. No command, Storage, Target or worker ownership.
    class LUX_RENDER_PUBLIC FrameProgress final
    {
    public:
        struct SlotSelection final
        {
            FrameContext frame;
            std::uint64_t epoch = 0;
        };

        FrameProgress(std::uint32_t frames_in_flight, const void* domain, std::size_t queues) noexcept;
        ~FrameProgress() noexcept;
        FrameProgress(const FrameProgress&) = delete;
        FrameProgress& operator=(const FrameProgress&) = delete;
        [[nodiscard]] RenderResult<std::optional<SlotSelection>> tryBegin(const DeviceSession&) noexcept;
        void adopt(SlotSelection, SubmissionPoint) noexcept;
        void endFrame(SlotSelection) noexcept;
        [[nodiscard]] bool canBegin(const SubmissionFrontiers& completed) const noexcept;
        [[nodiscard]] bool canBegin(const DeviceSession&) noexcept;
        [[nodiscard]] exec::task<RenderResult<SlotSelection>> acquireReplica(DeviceSession&) noexcept;
        [[nodiscard]] SubmissionFrontiers& replicaFrontiers(SlotSelection) noexcept;
        void endReplica(SlotSelection) noexcept;
        [[nodiscard]] RenderResult<bool> waitForProgress(DeviceSession&, std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] std::uint32_t nextSlot() const noexcept;
        [[nodiscard]] std::uint32_t frameCount() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }
        [[nodiscard]] const SubmissionFrontiers& frontiers(std::uint32_t slot) const noexcept { return slots_[slot]; }
        [[nodiscard]] std::span<const SubmissionFrontiers> frontiers() const noexcept { return slots_; }

        [[nodiscard]] std::uint64_t nextFrameIndex() const noexcept
        {
            return next_frame_index_;
        }

        [[nodiscard]] bool active() const noexcept
        {
            return access_ == ECpuAccess::FRAME;
        }

    private:
        enum class ECpuAccess : std::uint8_t { IDLE, FRAME, REPLICA };

        [[nodiscard]] SlotSelection beginCpuAccess(ECpuAccess) noexcept;
        void requireAccess(SlotSelection, ECpuAccess) const noexcept;
        void endCpuAccess(SlotSelection, ECpuAccess) noexcept;

        std::vector<SubmissionFrontiers> slots_;
        SubmissionFrontiers completed_;
        std::uint64_t next_frame_index_ = 0;
        std::uint64_t epoch_ = 0;
        std::uint32_t access_slot_ = 0;
        ECpuAccess access_ = ECpuAccess::IDLE;
        std::size_t wait_queue_cursor_ = 0;
    };
} // namespace lux::render::detail
