#pragma once

#include <lux/engine/render/resource/Storage.hpp>
#include <lux/engine/render/graph/Graph.hpp>

namespace lux::render
{
    enum class EResourceClaim : std::uint8_t { PROVIDE, REQUIRE, OPTIONAL };

    // Cold initializer for the currently implemented read-only scoped Buffer.
    // This is CPU source data, not a GPU owner or a universal Storage interface.
    struct BufferInitializer final
    {
        StorageTypeIdView type;
        cxx::SchemaVersion abi;
        RenderResult<std::vector<std::byte>> (*initialize)(std::uint64_t size) noexcept;
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
