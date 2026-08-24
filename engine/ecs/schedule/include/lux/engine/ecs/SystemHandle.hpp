#pragma once

#include <cstdint>

namespace lux::ecs
{
    class Schedule;
    class ScheduleEdit;

    template <class SystemType>
    class SystemHandle final
    {
      public:
        SystemHandle() noexcept = default;

        [[nodiscard]] bool valid() const noexcept
        {
            return owner_ != 0 && generation_ != 0;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return valid();
        }

      private:
        SystemHandle(
            std::uint64_t owner,
            std::uint32_t slot,
            std::uint32_t generation
        ) noexcept
            : owner_(owner), slot_(slot), generation_(generation)
        {
        }

        std::uint64_t owner_{};
        std::uint32_t slot_{};
        std::uint32_t generation_{};

        friend class Schedule;
        friend class ScheduleEdit;
    };
} // namespace lux::ecs
