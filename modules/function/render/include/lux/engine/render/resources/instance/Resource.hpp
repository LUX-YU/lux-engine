#pragma once

#include <lux/engine/render/resources/instance/Commands.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/BuiltinResources.hpp>

#include <lux/engine/render/resources/instance/Binding.hpp>

namespace lux::render
{
    class InstanceResource final
    {
    public:
        using Commands = std::tuple<
            InstanceCommands::CreateInstance, InstanceCommands::CreateInstances,
            InstanceCommands::ReleaseInstance, InstanceCommands::Update
        >;
        using PerFrameData = std::tuple<TransformUpdate, MeshInstanceUpdate>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kInstanceResourceName,
            0,
            0,
            cxx::make_basic_type_info<MeshInstanceBinding>(),
            "Paged raster transforms, indexed mesh/material references and live records; no skinning ABI.",
            EResourceDomain::SCENE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr)),
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };

        using Ops = void;
    };
} // namespace lux::render
