#pragma once

#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>

namespace lux::render::detail
{
    struct FrameContext final
    {
        std::uint64_t frame_index = 0;
        std::uint32_t slot_index = 0;
        bool operator==(const FrameContext&) const noexcept = default;
    };

    // DeviceEntry's strict round progress. No command, Storage, Target or worker ownership.
    class FrameProgress final
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
            void accept(SubmissionPoint accepted) noexcept;
            // Only after all CPU/native borrowers have returned; accepted prefixes survive failure.
            void finish() noexcept;

        private:
            friend class FrameProgress;
            FrameProgress* owner_ = nullptr;
            FrameContext frame_{};
        };

        FrameProgress(std::uint32_t frames_in_flight, const void* domain, std::size_t queues) noexcept;
        ~FrameProgress() noexcept;
        FrameProgress(const FrameProgress&) = delete;
        FrameProgress& operator=(const FrameProgress&) = delete;
        [[nodiscard]] RenderResult<bool> tryBegin(const SubmissionFrontiers& completed, Round& round) noexcept;

        [[nodiscard]] std::uint64_t nextFrameIndex() const noexcept
        {
            return next_frame_index_;
        }

        [[nodiscard]] bool active() const noexcept
        {
            return active_ != nullptr;
        }

    private:
        std::vector<SubmissionFrontiers> slots_;
        std::uint64_t next_frame_index_ = 0;
        Round* active_ = nullptr;
    };
} // namespace lux::render::detail
