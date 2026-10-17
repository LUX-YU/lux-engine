#pragma once

#include <lux/engine/render/resource/Mesh.hpp>
#include <lux/engine/render/resource/Material.hpp>
#include <optional>

namespace lux::render
{
    struct InstanceTag;
    using InstanceId = StorageResourceId<InstanceTag>;
    using TransformMatrix = std::array<float, 16>;
    inline constexpr TransformMatrix kIdentityTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    inline constexpr StorageNameView kInstanceStorageType("lux.render.InstanceStorage");

    struct InstanceDescription final
    {
        MeshId mesh;
        MaterialId material;
        std::uint32_t submesh = 0;
        TransformMatrix transform = kIdentityTransform;
    };

    struct InstancePublished final { InstanceId instance; std::uint64_t revision = 0; };
    struct InstanceInformation final { InstancePublished published; InstanceDescription description; };
    struct InstanceCreate final { StorageId storage; InstanceDescription description; };
    struct TransformUpdate final
    {
        InstanceId instance;
        std::uint64_t expected_revision = 0;
        TransformMatrix transform = kIdentityTransform;
    };

    struct TransformBatch final
    {
        StorageId storage;
        std::uint64_t sequence = 0; // Caller revision echoed exactly, never acknowledges later dirty state.
        std::vector<TransformUpdate> updates;
    };

    struct TransformApplied final { std::uint64_t sequence = 0; std::uint32_t changed = 0; };
    struct InstanceDrawGroup final
    {
        MeshId mesh;
        MaterialId material;
        std::uint32_t submesh = 0;
        bool operator==(const InstanceDrawGroup&) const noexcept = default;
    };

    enum class EInstanceBinding : std::uint8_t { MATRICES, DRAW };

    struct InstanceBinding final
    {
        std::uint32_t record_count = 0; // Cold declared prefix; growth never enlarges an existing Graph's range.
        std::optional<InstanceDrawGroup> group;
        EInstanceBinding role = EInstanceBinding::MATRICES;
        bool operator==(const InstanceBinding&) const noexcept = default;
    };

    struct CreateInstance final
    {
        static constexpr std::string_view name = "lux.render.CreateInstance";
        using Request = InstanceCreate;
        using Response = InstancePublished;
    };

    struct UpdateTransforms final
    {
        static constexpr std::string_view name = "lux.render.UpdateTransforms";
        using Request = TransformBatch;
        using Response = TransformApplied;
    };

    struct QueryInstance final
    {
        static constexpr std::string_view name = "lux.render.QueryInstance";
        using Request = InstanceId;
        using Response = InstanceInformation;
    };

    struct ReleaseInstance final
    {
        static constexpr std::string_view name = "lux.render.ReleaseInstance";
        using Request = InstancePublished;
        using Response = Ack;
    };

    template <> struct CommandValueTraits<TransformBatch> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const TransformBatch&) noexcept;
    };

    struct InstanceStorageConfig final
    {
        std::uint64_t resident_byte_budget = 0;
        bool operator==(const InstanceStorageConfig&) const noexcept = default;
    };

    class InstanceStorage final
    {
    public:
        using Config = InstanceStorageConfig;
        static inline const StorageDescriptor description{kInstanceStorageType, 0, 0,
            ConfigValue::typeOf<InstanceBinding>()};
        struct Ops final
        {
            RenderResult<InstanceInformation> (*query)(const void*, InstanceId) noexcept;
        };
        class Access final
        {
        public:
            [[nodiscard]] RenderResult<InstanceInformation> query(InstanceId id) const noexcept
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
}
