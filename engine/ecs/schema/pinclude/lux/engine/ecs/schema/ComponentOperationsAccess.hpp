#pragma once

#include <lux/engine/ecs/ComponentOperations.hpp>

namespace lux::ecs::detail
{
    struct ComponentOperationsAccess final
    {
        [[nodiscard]] static std::uint64_t storageKey(
            const ComponentOperations& operations
        ) noexcept
        {
            return operations.storage_key_;
        }

    };
} // namespace lux::ecs::detail
