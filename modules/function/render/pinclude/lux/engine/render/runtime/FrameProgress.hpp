#pragma once

#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/visibility.h>
#include <exec/task.hpp>

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
        // Embedded by the connected frame operation. Its stable lifetime covers Prepare through
        // the final Server adoption of native results, even while no Record worker is running.
        class Round final
        {
        public:
            Round() noexcept = default;
            ~Round() noexcept;
            Round(const Round&) = delete;
            Round& operator=(const Round&) = delete;

            [[nodiscard]] bool active() const noexcept
            {
                return owner_ != nullptr;
            }

            [[nodiscard]] FrameContext context() const noexcept;
            [[nodiscard]] const SubmissionFrontiers& accepted() const noexcept;
            void accept(SubmissionPoint accepted) noexcept;
            // Only after all CPU/native borrowers have returned; accepted prefixes survive failure.
            void finish() noexcept;

        private:
            friend class FrameProgress;
            FrameProgress* owner_ = nullptr;
            FrameContext frame_{};
        };

        // A structural Resource command writes one reusable replica. Its actual upload
        // frontier is folded into that physical slot without admitting a graphics round.
        class ReplicaWrite final
        {
        public:
            ReplicaWrite() noexcept = default;
            ~ReplicaWrite() noexcept;
            ReplicaWrite(const ReplicaWrite&) = delete;
            ReplicaWrite& operator=(const ReplicaWrite&) = delete;
            [[nodiscard]] std::uint32_t slot() const noexcept { return slot_; }
            [[nodiscard]] SubmissionFrontiers& accepted() noexcept { return accepted_; }

        private:
            friend class FrameProgress;
            FrameProgress* owner_ = nullptr;
            std::uint32_t slot_ = 0;
            SubmissionFrontiers accepted_;
        };

        FrameProgress(std::uint32_t frames_in_flight, const void* domain, std::size_t queues) noexcept;
        ~FrameProgress() noexcept;
        FrameProgress(const FrameProgress&) = delete;
        FrameProgress& operator=(const FrameProgress&) = delete;
        [[nodiscard]] RenderResult<bool> tryBegin(const SubmissionFrontiers& completed, Round& round) noexcept;
        [[nodiscard]] RenderResult<bool> tryBegin(const DeviceSession&, Round&) noexcept;
        [[nodiscard]] bool canBegin(const SubmissionFrontiers& completed) const noexcept;
        [[nodiscard]] bool canBegin(const DeviceSession&) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> acquireReplica(DeviceSession&, ReplicaWrite&) noexcept;
        [[nodiscard]] RenderResult<bool> waitForProgress(DeviceSession&, std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] std::uint32_t nextSlot() const noexcept;
        [[nodiscard]] std::uint32_t frameCount() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }
        [[nodiscard]] const SubmissionFrontiers& frontiers(std::uint32_t slot) const noexcept { return slots_[slot]; }

        [[nodiscard]] std::uint64_t nextFrameIndex() const noexcept
        {
            return next_frame_index_;
        }

        [[nodiscard]] bool active() const noexcept
        {
            return active_ != nullptr;
        }

    private:
        void advance() noexcept;

        std::vector<SubmissionFrontiers> slots_;
        SubmissionFrontiers completed_;
        std::uint64_t next_frame_index_ = 0;
        Round* active_ = nullptr;
        ReplicaWrite* writing_ = nullptr;
        std::size_t wait_queue_cursor_ = 0;
    };
} // namespace lux::render::detail
