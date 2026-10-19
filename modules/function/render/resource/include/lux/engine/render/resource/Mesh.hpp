#pragma once

#include <lux/engine/description/Image.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/resource/Storage.hpp>
#include <span>
#include <vector>

namespace lux::render
{
    class ResourceRegistry;
    struct MeshTag;
    using MeshId = StorageResourceId<MeshTag>;
    inline constexpr StorageNameView kMeshStorageType("lux.render.MeshStorage");

    // Asset layout, independent of the Shader's final descriptor placement and Pipeline state.
    struct MeshAttribute final
    {
        std::uint32_t location = 0, offset = 0;
        rdesc::ETextureFormat format = rdesc::ETextureFormat::UNDEFINED;
        bool operator==(const MeshAttribute&) const noexcept = default;
    };

    struct MeshSubmesh final
    {
        std::uint32_t first_index = 0, index_count = 0;
        std::int32_t vertex_offset = 0;
        bool operator==(const MeshSubmesh&) const noexcept = default;
    };

    struct MeshDescription final
    {
        std::uint32_t vertex_stride = 0, vertex_count = 0;
        std::uint64_t index_offset = 0;
        std::uint32_t index_stride = 4, index_count = 0;
        std::vector<MeshAttribute> attributes;
        std::vector<MeshSubmesh> submeshes;
        bool operator==(const MeshDescription&) const noexcept = default;
    };

    // Actual geometry record consumed when a Feature lowers a section to an indirect draw.
    // Instance count and selection belong to the Feature, not this asset table.
    struct MeshGpuSection final
    {
        std::uint32_t index_count = 0, first_index = 0;
        std::int32_t vertex_offset = 0;
    };

    static_assert(sizeof(MeshGpuSection) == 12);

    enum class EMeshBuffer : std::uint8_t
    {
        VERTICES,
        INDICES,
        SECTIONS
    };

    struct MeshBinding final
    {
        MeshId mesh;
        EMeshBuffer buffer = EMeshBuffer::VERTICES;
        std::uint32_t stride = 0;
        std::vector<MeshAttribute> attributes; // Vertex input ABI only; no counts, source offsets or Section mirror.
        bool operator==(const MeshBinding&) const noexcept = default;
    };

    struct MeshWrite final
    {
        StorageId storage;
        MeshDescription description;
        // Borrowed until Response (success, failure or cancellation). Failed submission takes no borrow.
        // Abandon/connection loss is not completion: Host must drain Server and Producers before freeing it.
        std::span<const std::byte> source;
    };

    struct CreateMesh final
    {
        static constexpr std::string_view name = "lux.render.CreateMesh";
        using Request = MeshWrite;
        using Response = RenderResult<MeshId>;
    };

    struct ReleaseMesh final
    {
        static constexpr std::string_view name = "lux.render.ReleaseMesh";
        using Request = MeshId;
        using Response = RenderResult<Ack>;
    };

    template <> struct ConfigValueTraits<MeshDescription> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const MeshDescription&) noexcept;
    };

    template <> struct ConfigValueTraits<MeshBinding> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const MeshBinding&) noexcept;
    };

    class MeshStorage final
    {
    public:
        using Config = void;
        static inline const StorageDescriptor description{kMeshStorageType, 0, 0, cppType<MeshBinding>()};

        using Ops = void;
    };
} // namespace lux::render
