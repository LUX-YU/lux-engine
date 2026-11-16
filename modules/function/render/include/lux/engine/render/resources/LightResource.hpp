#pragma once

#include <lux/engine/render/commands/Lighting3dCommands.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/BuiltinResources.hpp>

#include <lux/engine/render/extensions/resource/DirectLighting.hpp>

namespace lux::render
{
    struct LightResource final
    {
        using Commands = std::tuple<Lighting3dCommands::CreateLights, Lighting3dCommands::ReleaseLight>;
        using PerFrameData = std::tuple<LightUpdates>;
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            kLightResourceName,
            0,
            0,
            cxx::make_basic_type_info<DirectLightingBinding>(),
            "Scene direct-light GPU arrays: directional, finite point and spot lights, with independent FIF replicas.",
            EResourceDomain::SCENE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr)),
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
} // namespace lux::render
