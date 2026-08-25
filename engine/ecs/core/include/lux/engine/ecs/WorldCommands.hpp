#pragma once

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/core/visibility.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    namespace detail
    {
        class CommandShard;
    }

    enum class ECommandResult : std::uint8_t
    {
        ACCEPTED,
        STALE_WRITER,
        ALLOCATION_FAILURE,
    };

    class LUX_ENGINE_ECS_CORE_PUBLIC WorldCommands final
    {
      public:
        WorldCommands() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept;

        template <class Command>
            requires requires(Command& command, WorldMutation& edit)
            {
                { command.apply(edit) } noexcept -> std::same_as<void>;
            }
        [[nodiscard]] ECommandResult push(Command&& command) const noexcept
        {
            using Stored = std::remove_cvref_t<Command>;
            static_assert(std::is_rvalue_reference_v<Command&&>);
            static_assert(std::is_nothrow_move_constructible_v<Stored>);

            const CommandVTable table{
                sizeof(Stored),
                alignof(Stored),
                [](void* target, void* source) noexcept
                {
                    std::construct_at(
                        static_cast<Stored*>(target),
                        std::move(*static_cast<Stored*>(source))
                    );
                },
                [](void* payload, WorldMutation& edit) noexcept
                {
                    static_cast<Stored*>(payload)->apply(edit);
                },
                [](void* payload) noexcept
                {
                    std::destroy_at(static_cast<Stored*>(payload));
                }};

            return pushRaw(table, std::addressof(command));
        }

      private:
        struct CommandVTable final
        {
            std::size_t size{};
            std::size_t alignment{};
            void (*move_construct)(void*, void*) noexcept{};
            void (*apply)(void*, WorldMutation&) noexcept{};
            void (*destroy)(void*) noexcept{};
        };

        WorldCommands(
            detail::CommandShard& shard,
            std::uint32_t generation
        ) noexcept;

        [[nodiscard]] ECommandResult pushRaw(
            const CommandVTable& table,
            void* source
        ) const noexcept;

        detail::CommandShard* shard_{};
        std::uint32_t generation_{};

        friend class Schedule;
        friend class detail::CommandShard;
    };
} // namespace lux::ecs
