#pragma once

#include <lux/engine/render/commands/Mesh3dCommands.hpp>
#include <lux/engine/render/extensions/resource/IndexedTriangleMesh.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/BuiltinResources.hpp>

namespace lux::render
{
    class MeshResource final
    {
    public:
        using Commands = std::tuple<Mesh3dCommands::UploadMesh, Mesh3dCommands::ReleaseMesh>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kMeshResourceName,
            0,
            0,
            cxx::make_basic_type_info<MeshBinding>(),
            "Indexed triangle geometry with vertex and index arenas and section records.",
            EResourceDomain::DEVICE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr))
        };

        using Ops = void;
    };
} // namespace lux::render
