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
        using PerFrameData = std::tuple<MeshInstanceUpdate>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kInstanceResourceName,
            0,
            0,
            cxx::make_basic_type_info<MeshInstanceBinding>(),
            "Mesh, material and transform references with live records for raster draws.",
            EResourceDomain::SCENE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr)),
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };

        using Ops = void;
    };
} // namespace lux::render
