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

        [[nodiscard]] static bool defaultConstructible(
            const ComponentOperations& operations
        ) noexcept
        {
            return operations.default_emplace_ != nullptr;
        }

        [[nodiscard]] static void* defaultEmplace(
            const ComponentOperations& operations,
            WorldEdit& edit,
            Entity entity
        )
        {
            detail::require(operations.default_emplace_ != nullptr);
            return operations.default_emplace_(edit, entity);
        }
    };
} // namespace lux::ecs::detail
