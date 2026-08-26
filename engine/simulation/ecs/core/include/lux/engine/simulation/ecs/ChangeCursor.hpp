#pragma once

#include <cstdint>

namespace lux::simulation::ecs
{
    namespace detail
    {
        class EcsChangeLog;
        struct ChangeCursorAccess;
    }

    template <class Component>
    class ChangeCursor final
    {
      public:
        ChangeCursor() noexcept = default;

      private:
        std::uint64_t epoch_{};
        std::uint64_t sequence_{};

        friend class detail::EcsChangeLog;
        friend struct detail::ChangeCursorAccess;
    };

    class EntityChangeCursor final
    {
      public:
        EntityChangeCursor() noexcept = default;

      private:
        std::uint64_t epoch_{};
        std::uint64_t sequence_{};

        friend class detail::EcsChangeLog;
        friend struct detail::ChangeCursorAccess;
    };

    namespace detail
    {
        struct ChangeCursorAccess final
        {
            template <class Component>
            [[nodiscard]] static std::uint64_t& epoch(
                ChangeCursor<Component>& cursor
            ) noexcept
            {
                return cursor.epoch_;
            }

            template <class Component>
            [[nodiscard]] static std::uint64_t& sequence(
                ChangeCursor<Component>& cursor
            ) noexcept
            {
                return cursor.sequence_;
            }

            [[nodiscard]] static std::uint64_t& epoch(
                EntityChangeCursor& cursor
            ) noexcept
            {
                return cursor.epoch_;
            }

            [[nodiscard]] static std::uint64_t& sequence(
                EntityChangeCursor& cursor
            ) noexcept
            {
                return cursor.sequence_;
            }
        };
    } // namespace detail
} // namespace lux::simulation::ecs
