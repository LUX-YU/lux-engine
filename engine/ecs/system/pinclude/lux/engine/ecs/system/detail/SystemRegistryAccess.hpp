#pragma once

#include <lux/engine/ecs/SystemRegistry.hpp>

#include <memory>
#include <span>

namespace lux::ecs::detail
{
    struct SystemRegistryAccess final
    {
        [[nodiscard]] static std::shared_ptr<SystemRecord> record(
            const SystemRegistry& registry,
            SystemId id
        ) noexcept;

        [[nodiscard]] static std::span<const SystemId> ids(
            const SystemRegistry& registry
        ) noexcept;

        [[nodiscard]] static std::span<
            const std::shared_ptr<SystemRecord>
        > records(const SystemRegistry& registry) noexcept;
    };
}
