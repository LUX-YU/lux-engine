#pragma once

#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/features/FeaturePorts.hpp>

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
        // Config-dependent cold port information; this does not build a graph or create native objects.
        RenderResult<void> (*describe_ports)(const void*, std::string_view, FeaturePorts&) noexcept = nullptr;
    };
} // namespace lux::render
