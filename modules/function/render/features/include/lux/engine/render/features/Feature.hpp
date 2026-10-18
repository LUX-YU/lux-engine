#pragma once

#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>

namespace lux::render
{
    struct FeatureNameTag;
    using FeatureName = cxx::SchemaIdView<FeatureNameTag>;

    // Immutable author metadata; no execution, command registration or selection policy.
    struct FeatureDescriptor final
    {
        FeatureName type;
        CppType config;
    };
} // namespace lux::render
