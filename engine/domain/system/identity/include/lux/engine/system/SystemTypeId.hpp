#pragma once

#include <lux/engine/system/identity/visibility.h>

#include <lux/cxx/core/StableNameId.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace lux::system
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

    [[nodiscard]] LUX_ENGINE_SYSTEM_IDENTITY_PUBLIC SystemTypeId systemTypeId(std::string_view canonical_name);
} // namespace lux::system
