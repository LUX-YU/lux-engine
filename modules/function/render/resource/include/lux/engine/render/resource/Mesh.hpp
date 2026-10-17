#pragma once

#include <lux/engine/description/Image.hpp>
#include <lux/engine/render/data/Payload.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>
#include <lux/engine/render/resource/Storage.hpp>
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

    struct MeshPublished final { MeshId mesh; std::uint64_t revision = 0; };
    struct MeshBinding final
    {
        MeshId mesh;
        MeshDescription description; // The exact cold asset contract used to construct the Draw recipe.
        bool operator==(const MeshBinding&) const noexcept = default;
    };

    struct MeshWrite final
    {
        MeshId mesh;
        std::uint64_t expected_revision = 0;
        MeshDescription description;
        RenderPayload source;
    };

    struct MeshInformation final
    {
        MeshPublished published;
        MeshDescription description;
        std::uint64_t bytes = 0;
    };

    struct SetMesh final
    {
        static constexpr std::string_view name = "lux.render.SetMesh";
        using Request = MeshWrite;
        using Response = RenderResult<MeshPublished>;
    };

    struct QueryMesh final
    {
        static constexpr std::string_view name = "lux.render.QueryMesh";
        using Request = MeshId;
        using Response = RenderResult<MeshInformation>;
    };

    struct ReleaseMesh final
    {
        static constexpr std::string_view name = "lux.render.ReleaseMesh";
        using Request = MeshPublished;
        using Response = RenderResult<Ack>;
    };





    template <> struct ConfigValueTraits<MeshBinding> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const MeshBinding&) noexcept;
    };

    struct MeshStorageConfig final
    {
        std::uint64_t resident_byte_budget = 0; // Optional; includes retained physical versions.
        bool operator==(const MeshStorageConfig&) const noexcept = default;
    };

    class MeshStorage final
    {
    public:
        using Config = MeshStorageConfig;
        static inline const StorageDescriptor description{kMeshStorageType, 0, 0, ConfigValue::typeOf<MeshBinding>()};
        struct Ops final
        {
            RenderResult<MeshInformation> (*query)(const void*, MeshId) noexcept;
            RenderResult<void> (*release)(void*, MeshId, std::uint64_t) noexcept;
        };
        class Access final
        {
        public:
            [[nodiscard]] RenderResult<MeshInformation> query(MeshId id) const noexcept
            {
                return ops_->query(native_, id);
            }

            [[nodiscard]] RenderResult<void> release(MeshId id, std::uint64_t revision) const noexcept
            {
                if (!writable_) return cxx::unexpected(RenderError{kStorageIdentity});
                return ops_->release(native_, id, revision);
            }

        private:
            friend class ResourceRegistry;
            Access(void* native, const Ops& ops, bool writable) noexcept
                : native_(native), ops_(&ops), writable_(writable) {}
            void* native_;
            const Ops* ops_;
            bool writable_;
        };
    };
}
