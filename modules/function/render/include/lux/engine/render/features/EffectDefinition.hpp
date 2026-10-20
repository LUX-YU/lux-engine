#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>

namespace lux::render
{
    struct FeatureUse final
    {
        FeatureId feature;
        std::string contribution_key;
        ConfigValue config;
    };

    struct FeaturePortRef final
    {
        std::uint32_t contribution = 0;
        std::string port;
    };

    struct FeatureConnection final
    {
        FeaturePortRef source;
        FeaturePortRef destination;
    };

    struct FeatureInput final
    {
        FeaturePortRef destination;
        GraphResource resource;
    };

    struct FeatureExport final
    {
        FeaturePortRef source;
        std::optional<GraphOutputRequirements> target;
    };

    // Owned cold authoring input. IDs are resolved once by Runtime before composition.
    struct EffectDefinition final
    {
        std::vector<FeatureUse> features;
        std::vector<FeatureConnection> connections;
        std::vector<FeatureExport> outputs;
        std::vector<FeatureInput> inputs;
    };
} // namespace lux::render
