#pragma once

#include <lux/engine/render/resource/Binding.hpp>
#include <lux/engine/render/graph/Graph.hpp>

namespace lux::render
{
    enum class EResourceClaim : std::uint8_t { PROVIDE, REQUIRE, OPTIONAL };

    struct ResourceClaim final
    {
        std::string key;
        EResourceClaim participation = EResourceClaim::REQUIRE;
        VGraphResourceDesc description = GraphBufferDesc{};
        EGraphOrigin origin = EGraphOrigin::TRANSIENT;
        std::vector<VGraphRange> initial_ranges;
        std::optional<StorageRequirement> storage;
    };

    struct ResourceClaims final
    {
        std::vector<ResourceClaim> resources; // Graph-owned/transient or non-Storage graph inputs
        StorageGroups<ResourceClaim> storage; // explicit actual-owner requirements
    };

    // Borrowed only during build. Missing OPTIONAL ports have no entry.
    struct ResolvedResource final
    {
        std::string_view key;
        GraphVersion version;
    };

    // resolved is the key-sorted span supplied by assembleFeatures.
    [[nodiscard]] const ResolvedResource*
    findResource(std::span<const ResolvedResource> resolved, std::string_view key) noexcept;
}
