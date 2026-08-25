#pragma once

#include <lux/cxx/container/ScopeId.hpp>
#include <lux/cxx/container/SlotMap.hpp>

#include <cstdint>

namespace lux::ecs
{
    struct SystemRegistryScopeTag;
    using SystemRegistryId = lux::cxx::ScopeId<SystemRegistryScopeTag>;

    struct SystemSlotTag;
    using SystemSlot = lux::cxx::SlotKey<
        SystemSlotTag,
        std::uint32_t,
        std::uint32_t
    >;

    struct SystemId final
    {
        SystemRegistryId owner{};
        SystemSlot slot{};

        [[nodiscard]] constexpr bool isNull() const noexcept
        {
            return owner.isNull() || slot.isNull();
        }

        [[nodiscard]] constexpr bool isValid() const noexcept
        {
            return !isNull();
        }

        [[nodiscard]] constexpr bool operator==(
            const SystemId&
        ) const noexcept = default;
    };
}
