#pragma once

#include <cstdint>

namespace lux::ecs
{
    namespace detail
    {
        struct SystemHandleAccess;
    }

    class Schedule;
    class ScheduleEdit;

    class AnySystemHandle final
    {
      public:
        AnySystemHandle() noexcept = default;

        [[nodiscard]] bool valid() const noexcept
        {
            return owner_ != 0 && generation_ != 0;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return valid();
        }

        [[nodiscard]] bool operator==(
            const AnySystemHandle& other
        ) const noexcept = default;

      private:
        AnySystemHandle(
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
        friend struct detail::SystemHandleAccess;
    };

    template <class SystemType>
    class SystemHandle final
    {
      public:
        SystemHandle() noexcept = default;

        [[nodiscard]] bool valid() const noexcept
        {
            return handle_.valid();
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return valid();
        }

        [[nodiscard]] operator AnySystemHandle() const noexcept
        {
            return handle_;
        }

      private:
        explicit SystemHandle(AnySystemHandle handle) noexcept : handle_(handle) {}

        AnySystemHandle handle_{};

        friend class Schedule;
        friend class ScheduleEdit;
    };

    namespace detail
    {
        struct SystemHandleAccess final
        {
            [[nodiscard]] static std::uint64_t owner(
                AnySystemHandle handle
            ) noexcept
            {
                return handle.owner_;
            }

            [[nodiscard]] static std::uint32_t slot(
                AnySystemHandle handle
            ) noexcept
            {
                return handle.slot_;
            }

            [[nodiscard]] static std::uint32_t generation(
                AnySystemHandle handle
            ) noexcept
            {
                return handle.generation_;
            }

            [[nodiscard]] static AnySystemHandle make(
                std::uint64_t owner,
                std::uint32_t slot,
                std::uint32_t generation
            ) noexcept
            {
                return AnySystemHandle(owner, slot, generation);
            }
        };
    } // namespace detail
} // namespace lux::ecs
