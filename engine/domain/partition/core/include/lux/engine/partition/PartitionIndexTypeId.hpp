#pragma once

#include <lux/cxx/core/StableNameId.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace lux::partition
{
    struct PartitionIndexTypeId final
    {
        std::uint64_t hash{};
        std::string name;

        [[nodiscard]] bool valid() const noexcept
        {
            return !name.empty() && hash == lux::cxx::Fnv1a64::hash(name);
        }

        friend bool operator==(const PartitionIndexTypeId&, const PartitionIndexTypeId&) noexcept = default;
    };

    [[nodiscard]] inline PartitionIndexTypeId partitionIndexTypeId(std::string_view name)
    {
        return PartitionIndexTypeId{lux::cxx::Fnv1a64::hash(name), std::string(name)};
    }
}
