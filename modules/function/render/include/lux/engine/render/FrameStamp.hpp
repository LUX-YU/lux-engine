#pragma once
#include <cstdint>

namespace lux::render
{
    /// Strong-typed FIF slot index — prevents accidental mixing with serial.
    enum class FrameSlot : uint32_t{};

    /// Immutable per-tick frame identity produced by FrameClock.
    struct FrameStamp
    {
        uint64_t serial;           ///< Monotonically increasing frame number (starts at 1).
        FrameSlot slot;            ///< = serial % frames_in_flight (per-FIF array index).
        uint32_t frames_in_flight; ///< System FIF configuration value.
        uint32_t image_index;      ///< Swapchain image index for this tick.

        [[nodiscard]] uint32_t slotIndex() const noexcept
        {
            return static_cast<uint32_t>(slot);
        }
    };

    /// Single-source frame clock producing FrameStamp values.
    class FrameClock
    {
    public:
        explicit FrameClock(uint32_t fif) : frames_in_flight_(fif) {}

        /// Produce the stamp for the current tick and advance.
        FrameStamp beginTick(uint32_t image_index)
        {
            FrameStamp s{
                next_serial_,
                static_cast<FrameSlot>(next_serial_ % frames_in_flight_),
                frames_in_flight_,
                image_index};
            ++next_serial_;
            return s;
        }

        /// Serial of the most recently GPU-completed frame.
        /// After waiting fence[slot], frames older by FIF are guaranteed done.
        [[nodiscard]] uint64_t completedSerial(uint64_t current_serial) const noexcept
        {
            return (current_serial > frames_in_flight_)
                       ? current_serial - frames_in_flight_
                       : 0;
        }

        [[nodiscard]] uint32_t framesInFlight() const noexcept { return frames_in_flight_; }

    private:
        uint64_t next_serial_{1};
        uint32_t frames_in_flight_;
    };

} // namespace lux::render
