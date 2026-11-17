#pragma once

#include <lux/engine/render/resources/mesh/Commands.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/BuiltinResources.hpp>

namespace lux::render
{
    class MeshResource final
    {
    public:
        using Commands = std::tuple<MeshCommands::UploadMesh, MeshCommands::ReleaseMesh>;
        using PerFrameData = std::tuple<>;
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
