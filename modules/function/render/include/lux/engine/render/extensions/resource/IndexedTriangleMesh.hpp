#pragma once

#include <lux/engine/render/commands/Mesh3dCommands.hpp>

#include <lux/engine/render/resources/BuiltinResources.hpp>

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/inputs/IndexedTriangleMeshInputs.hpp>

namespace lux::render
{
    // Authoring metadata for this implementation's actual packed vertex ABI. Ordinary capability
    // users pass Mesh/Material identities; they do not choose the Storage's GPU layout.
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<MeshDescription> describeIndexedMesh(const rdesc::Mesh&) noexcept;

    enum class EMeshBuffer : std::uint8_t
    {
        VERTICES,
        INDICES,
        SECTIONS,
        BOUNDS
    };

    struct MeshBinding final
    {
        MeshId mesh;
        EMeshBuffer buffer = EMeshBuffer::VERTICES;
        std::uint32_t stride = 0;
        std::vector<MeshAttribute> attributes; // Vertex input ABI only; no counts, source offsets or Section mirror.
        bool operator==(const MeshBinding&) const noexcept = default;
    };

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

    template <> struct ConfigValueTraits<MeshBinding> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const MeshBinding&) noexcept;
    };
} // namespace lux::render
