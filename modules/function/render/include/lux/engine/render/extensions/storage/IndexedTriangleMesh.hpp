#pragma once

#include <lux/engine/render/extensions/storage/StorageDescriptor.hpp>
#include <lux/engine/render/inputs/IndexedTriangleMeshInputs.hpp>

namespace lux::render
{
    inline constexpr StorageNameView kMeshStorageType("lux.render.storage.IndexedTriangleMesh");

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

    class IndexedTriangleMeshStorageDescriptor final
    {
    public:
        using Config = void;
        static inline const StorageDescriptor descriptor{
            kMeshStorageType,
            0,
            0,
            cxx::typeToken<MeshBinding>(),
            "Indexed triangle geometry with vertex and index arenas and section records."
        };

        using Ops = void;
    };

    template <> struct ConfigValueTraits<MeshBinding> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const MeshBinding&) noexcept;
    };
} // namespace lux::render
