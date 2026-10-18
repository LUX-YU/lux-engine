#pragma once

#include <lux/engine/render/resource/Material.hpp>
#include <lux/engine/render/resource/Mesh.hpp>
#include <optional>
#include <tuple>

namespace lux::render
{
    struct MeshInstanceTag;
    using MeshInstanceId = StorageResourceId<MeshInstanceTag>;
    using TransformMatrix = std::array<float, 16>;
    inline constexpr TransformMatrix kIdentityTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    inline constexpr StorageNameView kMeshInstanceStorageType("lux.render.MeshInstanceStorage");

    struct MeshInstanceDescription final
    {
        MeshId mesh;
        MaterialId material;
        std::uint32_t submesh = 0;
        TransformMatrix transform = kIdentityTransform;
    };

    struct MeshInstancePublished final
    {
        MeshInstanceId instance;
        std::uint64_t revision = 0;
    };

    struct MeshInstanceInformation final
    {
        MeshInstancePublished published;
        MeshInstanceDescription description;
    };

    struct MeshInstanceCreate final
    {
        StorageId storage;
        MeshInstanceDescription description;
    };

    struct TransformUpdate final
    {
        MeshInstanceId instance;
        std::uint64_t expected_revision = 0;
        TransformMatrix transform = kIdentityTransform;
    };

    struct TransformBatch final
    {
        StorageId storage;
        std::uint64_t sequence = 0; // Caller revision echoed exactly, never acknowledges later dirty state.
        std::vector<TransformUpdate> updates;
    };

    struct TransformApplied final
    {
        std::uint64_t sequence = 0;
        std::uint32_t changed = 0;
    };

    struct MeshInstanceDrawGroup final
    {
        MeshId mesh;
        MaterialId material;
        std::uint32_t submesh = 0;
        bool operator==(const MeshInstanceDrawGroup&) const noexcept = default;

        bool operator<(const MeshInstanceDrawGroup& other) const noexcept
        {
            const auto key = [](const MeshInstanceDrawGroup& group) noexcept
            {
                return std::tuple{
                    group.mesh.storage.index,
                    group.mesh.storage.gen,
                    group.mesh.local.index,
                    group.mesh.local.gen,
                    group.material.storage.index,
                    group.material.storage.gen,
                    group.material.local.index,
                    group.material.local.gen,
                    group.submesh
                };
            };
            return key(*this) < key(other);
        }
    };

    enum class EMeshInstanceBinding : std::uint8_t
    {
        MATRICES,
        DRAW
    };

    struct MeshInstanceBinding final
    {
        std::uint32_t record_count = 0; // Cold declared prefix; growth never enlarges an existing Graph's range.
        std::optional<MeshInstanceDrawGroup> group;
        EMeshInstanceBinding role = EMeshInstanceBinding::MATRICES;
        bool operator==(const MeshInstanceBinding&) const noexcept = default;
    };

    struct CreateMeshInstance final
    {
        static constexpr std::string_view name = "lux.render.CreateMeshInstance";
        using Request = MeshInstanceCreate;
        using Response = RenderResult<MeshInstancePublished>;
    };

    struct UpdateTransforms final
    {
        static constexpr std::string_view name = "lux.render.UpdateTransforms";
        using Request = TransformBatch;
        using Response = RenderResult<TransformApplied>;
    };

    struct QueryMeshInstance final
    {
        static constexpr std::string_view name = "lux.render.QueryMeshInstance";
        using Request = MeshInstanceId;
        using Response = RenderResult<MeshInstanceInformation>;
    };

    struct ReleaseMeshInstance final
    {
        static constexpr std::string_view name = "lux.render.ReleaseMeshInstance";
        using Request = MeshInstancePublished;
        using Response = RenderResult<Ack>;
    };

    struct MeshInstanceStorageConfig final
    {
        std::uint64_t resident_byte_budget = 0;
        bool operator==(const MeshInstanceStorageConfig&) const noexcept = default;
    };

    class MeshInstanceStorage final
    {
    public:
        using Config = MeshInstanceStorageConfig;
        static inline const StorageDescriptor
            description{kMeshInstanceStorageType, 0, 0, cppType<MeshInstanceBinding>()};

        struct Ops final
        {
            RenderResult<MeshInstanceInformation> (*query)(const void*, MeshInstanceId) noexcept;
        };

        class Access final
        {
        public:
            [[nodiscard]] RenderResult<MeshInstanceInformation> query(MeshInstanceId id) const noexcept
            {
                return ops_->query(native_, id);
            }

        private:
            friend class ResourceRegistry;

            Access(void* native, const Ops& ops, bool) noexcept : native_(native), ops_(&ops) {}

            void* native_;
            const Ops* ops_;
        };
    };
} // namespace lux::render
