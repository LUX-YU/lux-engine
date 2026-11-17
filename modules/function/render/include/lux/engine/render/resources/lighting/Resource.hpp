#pragma once

#include <lux/engine/render/resources/lighting/Commands.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/BuiltinResources.hpp>

#include <lux/engine/render/resources/lighting/Binding.hpp>

namespace lux::render
{
    struct LightResource final
    {
        using Commands = std::tuple<
            LightingCommands::CreateLights, LightingCommands::ReleaseLight, LightingCommands::Update
        >;
        using PerFrameData = std::tuple<DirectLightUpdate>;
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
