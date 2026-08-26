#pragma once

#include <lux/engine/simulation/ecs/SimulationEcsMutation.hpp>
#include <lux/engine/simulation/ecs/core/visibility.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace lux::simulation::ecs
{
    namespace detail
    {
        class CommandShard;
    }

    enum class ECommandResult : std::uint8_t
    {
        ACCEPTED,
        STALE_WRITER,
        CAPACITY_EXCEEDED,
        BATCH_FAILED,
    };

    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsCommands final
    {
      public:
        EcsCommands() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept;

        template <class Command>
            requires requires(Command& command, SimulationEcsMutation& mutation)
            {
                { command.apply(mutation) } noexcept -> std::same_as<void>;
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
                [](void* payload, SimulationEcsMutation& mutation) noexcept
                {
                    static_cast<Stored*>(payload)->apply(mutation);
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
            void (*apply)(void*, SimulationEcsMutation&) noexcept{};
            void (*destroy)(void*) noexcept{};
        };

        EcsCommands(
            detail::CommandShard& shard,
            std::uint32_t generation
        ) noexcept;

        [[nodiscard]] ECommandResult pushRaw(
            const CommandVTable& table,
            void* source
        ) const noexcept;

        detail::CommandShard* shard_{};
        std::uint32_t generation_{};

        friend class detail::CommandShard;
    };
} // namespace lux::simulation::ecs
