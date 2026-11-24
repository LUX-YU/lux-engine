#pragma once

#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/device/SubmissionFrontiers.hpp>
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
        FrameProgress(std::uint32_t frames_in_flight, const void* domain, std::size_t queues) noexcept;
        ~FrameProgress() noexcept;
        FrameProgress(const FrameProgress&) = delete;
        FrameProgress& operator=(const FrameProgress&) = delete;
        [[nodiscard]] RenderResult<std::optional<FrameContext>> availableFrame(const DeviceSession&) noexcept;
        void beginFrame(FrameContext) noexcept;
        void adopt(FrameContext, SubmissionPoint) noexcept;
        void endFrame(FrameContext) noexcept;
        [[nodiscard]] RenderResult<bool> waitForProgress(DeviceSession&, std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] std::uint32_t nextSlot() const noexcept;
        [[nodiscard]] std::uint32_t frameCount() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }
        [[nodiscard]] const SubmissionFrontiers& frontiers(std::uint32_t slot) const noexcept { return slots_[slot]; }
        [[nodiscard]] std::span<const SubmissionFrontiers> frontiers() const noexcept { return slots_; }

        [[nodiscard]] bool active() const noexcept
        {
            return active_;
        }

    private:
        void requireFrame(FrameContext) const noexcept;

        std::vector<SubmissionFrontiers> slots_;
        SubmissionFrontiers completed_;
        std::uint64_t next_frame_index_ = 0;
        bool active_ = false;
        std::size_t wait_queue_cursor_ = 0;
    };
} // namespace lux::render::detail
