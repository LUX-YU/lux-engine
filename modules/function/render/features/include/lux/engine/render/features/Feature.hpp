#pragma once

#include <lux/engine/render/data/Schema.hpp>

namespace lux::render
{
    struct FeatureTypeTag;
    using FeatureTypeIdView = cxx::SchemaIdView<FeatureTypeTag>;

    // Immutable author metadata; no execution, command registration or selection policy.
    struct FeatureDescriptor final
    {
        FeatureTypeIdView type;
        cxx::SchemaVersion version;
        const DataSchemaDescriptor* config_schema = nullptr;
    };
}
