#pragma once

#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/graph/Graph.hpp>

namespace lux::render
{
    struct StorageTypeTag;
    using StorageTypeId = cxx::SchemaId<StorageTypeTag>;
    using StorageTypeIdView = cxx::SchemaIdView<StorageTypeTag>;

    enum class EResourceClaim : std::uint8_t { PROVIDE, REQUIRE, OPTIONAL };
    enum class EResourceScope : std::uint8_t { GLOBAL, SCENE, VIEW, ATTACHMENT };

    struct StorageRequirement final
    {
        StorageTypeId type;
        cxx::SchemaVersion abi;
        EResourceScope scope = EResourceScope::GLOBAL;
        bool operator==(const StorageRequirement&) const noexcept = default;
    };

    struct ResourceClaim final
    {
        std::string key;
        EResourceClaim participation = EResourceClaim::REQUIRE;
        VGraphResourceDesc description = GraphBufferDesc{};
        EGraphOrigin origin = EGraphOrigin::TRANSIENT;
        std::vector<VGraphRange> initial_ranges;
        std::optional<StorageRequirement> storage;
    };

    // Borrowed only during build. Missing OPTIONAL ports have no entry.
    struct ResolvedResource final
    {
        std::string_view key;
        GraphVersion version;
    };

    struct ResolvedStorageBinding final
    {
        GraphResourceId resource;
        StorageRequirement storage;
    };

    // resolved is the key-sorted span supplied by assembleFeatures.
    [[nodiscard]] const ResolvedResource*
    findResource(std::span<const ResolvedResource> resolved, std::string_view key) noexcept;
}
