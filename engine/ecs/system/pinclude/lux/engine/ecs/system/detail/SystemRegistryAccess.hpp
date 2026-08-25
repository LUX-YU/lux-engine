#pragma once

#include <lux/engine/ecs/SystemRegistry.hpp>

#include <span>

namespace lux::ecs::detail
{
    struct LUX_ENGINE_ECS_SYSTEM_PUBLIC SystemRegistryAccess final
    {
        [[nodiscard]] static SystemRegistryId scope(
            const SystemRegistry& registry
        ) noexcept;

        [[nodiscard]] static const SystemRecord* record(
            const SystemRegistry& registry,
            SystemId id
        ) noexcept;

        [[nodiscard]] static SystemRecord* record(
            SystemRegistry& registry,
            SystemId id
        ) noexcept;

        [[nodiscard]] static SystemRecord* record(
            SystemRegistry& registry,
            SystemSlot slot
        ) noexcept;

        [[nodiscard]] static std::span<const SystemSlot> slots(
            const SystemRegistry& registry
        ) noexcept;

        [[nodiscard]] static std::span<const SystemRecord> records(
            const SystemRegistry& registry
        ) noexcept;

        [[nodiscard]] static bool acquireExecution(
            SystemRegistry& registry
        ) noexcept;

        static void releaseExecution(SystemRegistry& registry) noexcept;
    };
}
