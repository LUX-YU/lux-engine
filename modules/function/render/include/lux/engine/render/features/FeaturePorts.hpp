#pragma once

#include <lux/engine/render/graph/GraphResource.hpp>
#include <lux/engine/render/resource/Binding.hpp>

namespace lux::render
{
    enum class EFeaturePort : std::uint8_t
    {
        OUTPUT,
        INPUT,
        OPTIONAL
    };

    struct FeaturePort final
    {
        std::string key;
        EFeaturePort direction = EFeaturePort::INPUT;
        VGraphResourceDesc description = GraphBufferDesc{};
        EGraphOrigin origin = EGraphOrigin::TRANSIENT;
        std::vector<VGraphRange> initial_ranges;
        std::optional<StorageRequirement> storage;
    };

    struct FeaturePorts final
    {
        std::vector<FeaturePort> resources;
        StorageGroups<FeaturePort> storage;
    };

    struct ResolvedResource final
    {
        std::string_view key;
        ResourceVersion version;
    };
} // namespace lux::render
