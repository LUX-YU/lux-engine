#pragma once

#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <lux/engine/render/extensions/storage/Binding.hpp>

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
        EGraphUse initial_use = EGraphUse::UNSPECIFIED;
        std::string semantic; // Optional full encoding identity; consumers that require it compare the whole name.
    };

    struct FeatureHistory final
    {
        std::string previous, current;
        HistoryInitialValue initial;
        EGraphUse canonical_use = EGraphUse::SAMPLED;
        std::uint32_t distance = 1;
    };

    struct FeaturePorts final
    {
        std::vector<FeaturePort> resources;
        StorageGroups<FeaturePort> storage;
        std::vector<FeatureHistory> histories;
    };

    struct ResolvedResource final
    {
        std::string_view key;
        ResourceVersion version;
    };
} // namespace lux::render
