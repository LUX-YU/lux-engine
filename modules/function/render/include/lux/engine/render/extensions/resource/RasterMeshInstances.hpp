#pragma once

#include <lux/engine/render/commands/Mesh3dCommands.hpp>

#include <lux/engine/render/resources/BuiltinResources.hpp>

#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>

namespace lux::render
{
    enum class EMeshInstanceBinding : std::uint8_t
    {
        TRANSFORMS,
        PROPERTIES,
        ALIVE
    };

    struct MeshInstanceBinding final
    {
        EMeshInstanceBinding role = EMeshInstanceBinding::TRANSFORMS;
        bool operator==(const MeshInstanceBinding&) const noexcept = default;
    };

    class InstanceResource final
    {
    public:
        using Commands = std::tuple<Mesh3dCommands::CreateInstance, Mesh3dCommands::CreateInstances, Mesh3dCommands::ReleaseInstance>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kInstanceResourceName,
            0,
            0,
            cxx::make_basic_type_info<MeshInstanceBinding>(),
            "Paged raster transforms, indexed mesh/material references and live records; no skinning ABI.",
            EResourceDomain::SCENE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr))
        };

        using Ops = void;
    };
} // namespace lux::render
