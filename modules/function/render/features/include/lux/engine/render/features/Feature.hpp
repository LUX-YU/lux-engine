#pragma once

#include <lux/engine/render/data/Schema.hpp>
#include <lux/engine/render/core/Ids.hpp>

namespace lux::render
{
    struct FeatureNameTag;
    using FeatureName = cxx::SchemaIdView<FeatureNameTag>;

    // Immutable author metadata; no execution, command registration or selection policy.
    struct FeatureDescriptor final
    {
        FeatureName type;
        cxx::SchemaVersion version;
        const DataSchemaDescriptor* config_schema = nullptr;
    };
}
