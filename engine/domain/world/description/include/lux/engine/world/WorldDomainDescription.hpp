#pragma once

#include <lux/engine/world/WorldPartition.hpp>

namespace lux::world
{
    inline constexpr std::string_view kDefaultWorldDomain = "default";

    // Authored algorithm choice before a World partition table exists.
    struct WorldDomainSelection final
    {
        std::string key;
        WorldPartitionerDescriptor partitioner;
        std::uint32_t configuration_version{};
        std::vector<std::byte> configuration;
    };

    // One exact-cover interval in the containing World's partition table. Index
    // roots refer to that World's storage volumes and bundle generation.
    // The key is author identity; it does not name a geometry or runtime System.
    struct WorldDomainDescription final
    {
        std::string key;
        WorldPartitionerDescriptor partitioner;
        partition::PartitionOrdinal first;
        std::uint32_t count{};
        std::uint32_t configuration_version{};
        std::vector<std::byte> configuration;
        std::vector<WorldPartitionIndexDescription> indexes;
    };
} // namespace lux::world
