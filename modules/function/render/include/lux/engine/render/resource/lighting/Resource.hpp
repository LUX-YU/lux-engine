#pragma once

#include <lux/engine/render/resource/lighting/Data.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>

#include <lux/engine/render/resource/lighting/Binding.hpp>

namespace lux::render
{
    struct LightResource final
    {
        using PerFrameData = std::tuple<DirectLightUpdate>;
        using Config = void;

        static inline const ResourceDefinition descriptor{
            kLightResourceName,
            {},
            {},
            cxx::make_basic_type_info<DirectLightingBinding>(),
            "Scene direct-light GPU arrays: directional, finite point and spot lights, with independent FIF replicas.",
            EResourceDomain::SCENE,
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
} // namespace lux::render
