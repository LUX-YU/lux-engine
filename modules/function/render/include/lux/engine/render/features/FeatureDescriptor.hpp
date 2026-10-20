#pragma once

#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>

namespace lux::render
{
    struct FeatureNameTag;
    using FeatureName = cxx::SchemaIdView<FeatureNameTag>;

    // Immutable author metadata; no execution, command registration or selection policy.
    struct FeatureDescriptor final
    {
        FeatureName type;
        CppType config;
        std::string_view description;
    };
} // namespace lux::render
