#pragma once

#include <lux/engine/authoring/world/visibility.h>

#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/authoring/world/WorldPartition.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>

#include <uuid.h>

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lux::authoring
{
    inline constexpr std::uint32_t kWorldSourceMagic = 0x4157584cu;
    inline constexpr std::uint32_t kWorldSourceVersion = 4u;
    inline constexpr std::uint32_t kWorldDescriptorPageMagic = 0x4941584cu;
    inline constexpr std::uint32_t kWorldDescriptorPageVersion = 2u;
    inline constexpr std::uint32_t kWorldActorDocumentMagic = 0x4441584cu;
    inline constexpr std::uint32_t kWorldActorDocumentVersion = 2u;
    inline constexpr std::uint32_t kWorldInstancePageMagic = 0x5049584cu;
    inline constexpr std::uint32_t kWorldInstancePageVersion = 2u;
    inline constexpr std::uint32_t kWorldTerrainPageMagic = 0x5054584cu;
    inline constexpr std::uint32_t kWorldTerrainPageVersion = 1u;
    inline constexpr std::uint32_t kWorldTilePageMagic = 0x4c54584cu;
    inline constexpr std::uint32_t kWorldTilePageVersion = 1u;
    inline constexpr std::uint32_t kWorldPixelPageMagic = 0x5050584cu;
    inline constexpr std::uint32_t kWorldPixelPageVersion = 1u;
    inline constexpr std::uint32_t kWorldLogicalChunkEdge = 256u;
    inline constexpr std::uint32_t kWorldTerrainSampleEdge = 257u;

    enum class EWorldActorReferenceKind : std::uint8_t
    {
        LOCAL,
        REQUIRED,
        OPTIONAL_REFERENCE
    };

    struct WorldActorSourceReference final
    {
        WorldActorId target;
        EWorldActorReferenceKind kind{
            EWorldActorReferenceKind::OPTIONAL_REFERENCE};

        friend bool operator==(
            const WorldActorSourceReference&,
            const WorldActorSourceReference&) = default;
    };

    struct WorldActorComponentRecord final
    {
        std::string schema_name;
        std::uint32_t schema_version{1u};
        std::vector<std::byte> tagged_payload;

        friend bool operator==(
            const WorldActorComponentRecord&,
            const WorldActorComponentRecord&) = default;
    };

    using WorldActorSourcePosition = std::variant<
        lux::math::Position2d,
        lux::math::Position3d>;

    /// LXAD v2 is an Authoring-only, single-Actor tagged document. The opaque
    /// NameTable is required by TaggedPropertyReader; Toolchain validates and
    /// remaps the records directly into LXES component columns.
    struct WorldActorDocument final
    {
        WorldId world;
        WorldActorId actor;
        std::string actor_class;
        lux::authoring::PartitionSpaceId space;
        WorldActorSourcePosition position;
        /// Stable Transform parent. Empty means this Actor is a registry-space
        /// root. `position` is always the absolute placement used by the
        /// Authoring partition index; a Transform component may additionally
        /// carry the relative value used after parent relocation.
        std::optional<WorldActorId> transform_parent;
        std::vector<lux::authoring::DataLayerId> data_layers;
        std::vector<WorldActorSourceReference> references;
        std::vector<std::byte> name_table;
        std::vector<WorldActorComponentRecord> components;

        friend bool operator==(
            const WorldActorDocument&,
            const WorldActorDocument&) = default;
    };

    struct EditableWorldInstance final
    {
        lux::authoring::WorldInstanceId id;
        WorldActorSourcePosition position;
        std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 3u> scale{1.0f, 1.0f, 1.0f};
        lux::asset::asset_id_t mesh{};
        lux::asset::asset_id_t material_instance{};
        std::uint32_t rgba8{0xffffffffu};
        std::array<std::array<float, 4>, 4> custom_values{};
        std::vector<lux::authoring::DataLayerId> data_layers;
        std::uint32_t editor_flags{0u};

        friend bool operator==(
            const EditableWorldInstance&,
            const EditableWorldInstance&) = default;
    };

    struct WorldInstancePageDocument final
    {
        WorldId world;
        lux::authoring::InstanceSetId instance_set;
        lux::authoring::PartitionSpaceId space;
        lux::authoring::WorldCellKey cell;
        std::vector<EditableWorldInstance> instances;
        std::vector<std::uint64_t> tombstones;

        friend bool operator==(
            const WorldInstancePageDocument&,
            const WorldInstancePageDocument&) = default;
    };

    struct WorldTerrainPageDocument final
    {
        WorldId world;
        lux::authoring::TerrainSetId terrain_set;
        lux::authoring::PartitionSpaceId space;
        lux::authoring::WorldCellKey cell;
        float height_min{0.0f};
        float height_max{1.0f};
        float sample_spacing{1.0f};
        std::vector<float> heights;
        std::uint8_t weight_layer_count{0u};
        std::array<std::vector<std::uint8_t>, 2> weight_planes;
        std::vector<std::uint8_t> holes;

        friend bool operator==(
            const WorldTerrainPageDocument&,
            const WorldTerrainPageDocument&) = default;
    };

    struct WorldTileCollisionBox final
    {
        std::uint16_t x{0u};
        std::uint16_t y{0u};
        std::uint16_t width{1u};
        std::uint16_t height{1u};

        friend bool operator==(
            const WorldTileCollisionBox&,
            const WorldTileCollisionBox&) = default;
    };

    struct WorldTilePageDocument final
    {
        WorldId world;
        lux::authoring::TilemapId tilemap;
        lux::authoring::PartitionSpaceId space;
        lux::authoring::WorldCellKey cell;
        lux::asset::asset_id_t tileset{};
        std::uint32_t tileset_columns{1u};
        std::uint32_t tileset_rows{1u};
        std::array<float, 2> tile_size{1.0f, 1.0f};
        std::vector<std::uint32_t> tile_ordinals;
        std::vector<WorldTileCollisionBox> collision_boxes;

        friend bool operator==(
            const WorldTilePageDocument&,
            const WorldTilePageDocument&) = default;
    };

    struct WorldPixelGeneratorSource final
    {
        lux::authoring::ChunkGeneratorId id;
        std::uint32_t schema_version{1u};
        std::uint32_t config_schema_version{1u};
        std::uint64_t seed{0u};
        std::vector<std::byte> config;

        friend bool operator==(
            const WorldPixelGeneratorSource&,
            const WorldPixelGeneratorSource&) = default;
    };

    struct WorldPixelPageDocument final
    {
        WorldId world;
        lux::authoring::PixelFieldId field;
        lux::authoring::PartitionSpaceId space;
        lux::authoring::WorldCellKey cell;
        std::vector<std::uint16_t> material_base;
        std::optional<WorldPixelGeneratorSource> generator;

        friend bool operator==(
            const WorldPixelPageDocument&,
            const WorldPixelPageDocument&) = default;
    };

    /// Lightweight descriptor kept in the root index. Component payload lives
    /// in `document_path`, so the Editor Outliner can list unloaded Actors
    /// without materializing their ECS proxy.
    struct WorldActorSourceDescriptor final
    {
        WorldActorId id;
        std::string display_name;
        std::string actor_class;
        std::string document_path;
        lux::cxx::algorithm::Sha256Digest content_digest;
        lux::authoring::PartitionSpaceId space;
        WorldActorSourcePosition position;
        std::optional<WorldActorId> transform_parent;
        std::array<float, 3u> bounds_half_extent{};
        std::vector<lux::authoring::DataLayerId> data_layers;
        std::vector<WorldActorSourceReference> references;

        friend bool operator==(
            const WorldActorSourceDescriptor&,
            const WorldActorSourceDescriptor&) = default;
    };

    enum class EWorldPageSourceKind : std::uint8_t
    {
        INSTANCE,
        TERRAIN,
        TILE,
        PIXEL
    };

    using WorldPageSourceOwner = std::variant<
        lux::authoring::InstanceSetId,
        lux::authoring::TerrainSetId,
        lux::authoring::TilemapId,
        lux::authoring::PixelFieldId>;

    struct WorldPageSourceDescriptor final
    {
        uuids::uuid id{};
        EWorldPageSourceKind kind{EWorldPageSourceKind::INSTANCE};
        WorldPageSourceOwner owner{lux::authoring::InstanceSetId{}};
        std::string document_path;
        lux::authoring::PartitionSpaceId space;
        lux::authoring::WorldCellKey cell;
        lux::cxx::algorithm::Sha256Digest content_digest;

        friend bool operator==(
            const WorldPageSourceDescriptor&,
            const WorldPageSourceDescriptor&) = default;
    };

    struct WorldDescriptorPageReference final
    {
        uuids::uuid id{};
        lux::authoring::PartitionSpaceId space;
        lux::authoring::WorldMacroCoord macro;
        std::string document_path;
        lux::cxx::algorithm::Sha256Digest content_digest;
        std::uint32_t actor_count{0u};
        std::uint32_t page_count{0u};

        friend bool operator==(
            const WorldDescriptorPageReference&,
            const WorldDescriptorPageReference&) = default;
    };

    /// Authoritative monotonic identity allocator for one Instance Set.
    /// Keeping this once in LXWA prevents two independently edited LXIP Cell
    /// pages from issuing the same WorldInstanceId.
    struct WorldInstanceSetSourceDescriptor final
    {
        lux::authoring::InstanceSetId id;
        std::uint64_t next_local_id{1u};

        friend bool operator==(
            const WorldInstanceSetSourceDescriptor&,
            const WorldInstanceSetSourceDescriptor&) = default;
    };

    struct WorldSceneFeatureRequest final
    {
        WorldSceneFeatureId id;
        std::uint32_t config_schema_version{0u};
        std::vector<std::byte> config;

        friend bool operator==(
            const WorldSceneFeatureRequest&,
            const WorldSceneFeatureRequest&) = default;
    };

    struct WorldRequiredExtension final
    {
        WorldExtensionId id;
        std::uint16_t required_major{0u};
        std::uint16_t minimum_minor{0u};

        friend bool operator==(
            const WorldRequiredExtension&,
            const WorldRequiredExtension&) = default;
    };

    struct WorldSourceDocument final
    {
        WorldId world;
        /// Dimension-neutral scene features. Spatial selection, render,
        /// physics and navigation configuration belongs to each feature,
        /// never to this Authoring root.
        std::vector<WorldSceneFeatureRequest> contributions;
        std::vector<lux::authoring::PartitionSpaceDescriptor> spaces;
        /// Authoring membership vocabulary only. Runtime loading/activation
        /// policy belongs to a contribution, not to the LXWA root.
        std::vector<lux::authoring::DataLayerId> data_layers;
        std::vector<WorldRequiredExtension> required_extensions;
        std::vector<WorldInstanceSetSourceDescriptor> instance_sets;
        std::vector<WorldDescriptorPageReference> descriptor_pages;
    };

    struct WorldDescriptorPageDocument final
    {
        WorldId world;
        uuids::uuid id{};
        lux::authoring::PartitionSpaceId space;
        lux::authoring::WorldMacroCoord macro;
        std::vector<WorldActorSourceDescriptor> actors;
        std::vector<WorldPageSourceDescriptor> pages;

        friend bool operator==(
            const WorldDescriptorPageDocument&,
            const WorldDescriptorPageDocument&) = default;
    };
} // namespace lux::authoring
