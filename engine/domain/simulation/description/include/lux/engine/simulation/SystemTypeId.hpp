#pragma once

#include <lux/engine/simulation/description/visibility.h>

#include <lux/cxx/core/StableNameId.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace lux::simulation
{
    struct SystemTypeId final
    {
        std::uint64_t hash{};
        std::string name;

        [[nodiscard]] bool valid() const noexcept
        {
            return !name.empty() && hash == lux::cxx::Fnv1a64::hash(name);
        }

        friend bool operator==(const SystemTypeId&, const SystemTypeId&) noexcept = default;
    };

    struct SystemTypeIdLess final
    {
        [[nodiscard]] bool operator()(const SystemTypeId& left, const SystemTypeId& right) const noexcept
        {
            return left.hash < right.hash || (left.hash == right.hash && left.name < right.name);
        }
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC SystemTypeId systemTypeId(std::string_view canonical_name);
}
