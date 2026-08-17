#pragma once

#include <lux/engine/authoring/world/WorldSource.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::authoring
{
    struct WorldSourceCodecLimits final
    {
        std::uint64_t maximum_bytes{256u * 1024u * 1024u};
        std::uint64_t maximum_descriptor_page_bytes{
            16u * 1024u * 1024u};
        std::uint32_t maximum_spaces{4096u};
        std::uint32_t maximum_data_layers{65536u};
        std::uint32_t maximum_requirements{65536u};
        std::uint32_t maximum_contributions{65536u};
        std::uint32_t maximum_instance_sets{4u * 1024u * 1024u};
        std::uint32_t maximum_generator_config_bytes{64u * 1024u};
        std::uint32_t maximum_descriptor_pages{4u * 1024u * 1024u};
        std::uint32_t maximum_actors{4u * 1024u * 1024u};
        std::uint32_t maximum_pages{4u * 1024u * 1024u};
        std::uint32_t maximum_actor_references{65536u};
        std::uint32_t maximum_components_per_actor{65536u};
        std::uint32_t maximum_instances_per_page{4u * 1024u * 1024u};
        std::uint32_t maximum_string_bytes{4096u};
    };

    struct WorldSourceGarbageCollectionConfig final
    {
        std::chrono::seconds grace_period{std::chrono::hours{24}};
        std::uint32_t maximum_removals_per_pass{4096u};
    };

    struct WorldSourceGarbageCollectionResult final
    {
        std::uint64_t live_documents{0u};
        std::uint64_t scanned_documents{0u};
        std::uint64_t removed_documents{0u};
        std::uint64_t deferred_documents{0u};
        bool removal_budget_exhausted{false};
    };

    /// Creates a new empty Authoring scene with a random identity and one
    /// default Partition Space. Topology belongs to that space rather than to
    /// a top-level 2D/3D World discriminator.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    WorldSourceDocument makeWorldSourceDocument(lux::authoring::EPartitionTopology topology);

    /// Canonical content-addressed location for one external Actor document.
    /// Keeping this in Authoring prevents Editor workflows from inventing
    /// subtly different directory layouts for the same World contract.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    std::string makeWorldActorDocumentPath(
        lux::entity_scene::PersistentEntityId actor,
        const lux::cxx::algorithm::Sha256Digest& content_digest);

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    uuids::uuid makeWorldDescriptorPageId(
        lux::entity_scene::EntitySceneId world,
        lux::authoring::PartitionSpaceId space,
        const lux::authoring::WorldMacroCoord& macro);

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    std::string makeWorldDescriptorPagePath(
        const uuids::uuid& page,
        const lux::cxx::algorithm::Sha256Digest& content_digest);

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    std::string makeWorldInstancePagePath(
        lux::authoring::InstanceSetId instance_set,
        const lux::authoring::WorldCellKey& cell,
        const lux::cxx::algorithm::Sha256Digest& content_digest);

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC std::string
    makeWorldTerrainPagePath(
        lux::authoring::TerrainSetId terrain,
        const lux::authoring::WorldCellKey& cell,
        const lux::cxx::algorithm::Sha256Digest& content_digest);
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC std::string
    makeWorldTilePagePath(
        lux::authoring::TilemapId tilemap,
        const lux::authoring::WorldCellKey& cell,
        const lux::cxx::algorithm::Sha256Digest& content_digest);
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC std::string
    makeWorldPixelPagePath(
        lux::authoring::PixelFieldId field,
        const lux::authoring::WorldCellKey& cell,
        const lux::cxx::algorithm::Sha256Digest& content_digest);

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldSource(const WorldSourceDocument& document) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldSourceDocument, std::string>
    decodeWorldSource(
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldDescriptorPage(
        const WorldSourceDocument& root,
        const WorldDescriptorPageDocument& page) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldDescriptorPageDocument, std::string>
    decodeWorldDescriptorPage(
        const WorldSourceDocument& root,
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldActorDocument(const WorldActorDocument& document) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldActorDocument, std::string>
    decodeWorldActorDocument(
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldInstancePage(
        const WorldSourceDocument& root,
        const WorldInstancePageDocument& page) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldInstancePageDocument, std::string>
    decodeWorldInstancePage(
        const WorldSourceDocument& root,
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldInstancePageDocument, std::string>
    loadWorldInstancePage(
        const std::filesystem::path& root_document,
        std::string_view relative_path,
        const WorldSourceDocument& root,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldTerrainPage(
        const WorldSourceDocument& root,
        const WorldTerrainPageDocument& page) noexcept;
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldTerrainPageDocument, std::string>
    decodeWorldTerrainPage(
        const WorldSourceDocument& root,
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldTerrainPageDocument, std::string>
    loadWorldTerrainPage(
        const std::filesystem::path& root_document,
        std::string_view relative_path,
        const WorldSourceDocument& root,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldTilePage(
        const WorldSourceDocument& root,
        const WorldTilePageDocument& page) noexcept;
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldTilePageDocument, std::string>
    decodeWorldTilePage(
        const WorldSourceDocument& root,
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldTilePageDocument, std::string>
    loadWorldTilePage(
        const std::filesystem::path& root_document,
        std::string_view relative_path,
        const WorldSourceDocument& root,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldPixelPage(
        const WorldSourceDocument& root,
        const WorldPixelPageDocument& page) noexcept;
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldPixelPageDocument, std::string>
    decodeWorldPixelPage(
        const WorldSourceDocument& root,
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldPixelPageDocument, std::string>
    loadWorldPixelPage(
        const std::filesystem::path& root_document,
        std::string_view relative_path,
        const WorldSourceDocument& root,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldActorDocument, std::string>
    loadWorldActorDocument(
        const std::filesystem::path& root_document,
        const WorldActorSourceDescriptor& descriptor,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<void, std::string> saveWorldSource(
        const std::filesystem::path& path,
        const WorldSourceDocument& document) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldSourceDocument, std::string> loadWorldSource(
        const std::filesystem::path& path,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldDescriptorPageDocument, std::string>
    loadWorldDescriptorPage(
        const std::filesystem::path& root_document,
        const WorldSourceDocument& root,
        const WorldDescriptorPageReference& reference,
        const WorldSourceCodecLimits& limits = {}) noexcept;

    /// Resolves one validated root-relative external document path. Absolute,
    /// parent-traversing and symlink-escaping paths are rejected.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<std::filesystem::path, std::string>
    resolveWorldSourceDocument(
        const std::filesystem::path& root_document,
        std::string_view relative_path) noexcept;

    /// Atomically writes an external Actor/Page document below the LXWA root.
    /// The caller still commits the root last, after every child succeeded.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<void, std::string> saveWorldSourceDocument(
        const std::filesystem::path& root_document,
        std::string_view relative_path,
        std::span<const std::byte> bytes) noexcept;

    /// Rebuild the authoritative live-set from the committed LXWA Root and
    /// its digest-verified LXAI pages, then remove only known content-object
    /// extensions outside that set and older than the grace period. No broad
    /// directory removal or symlink traversal is performed. A malformed live
    /// page aborts the pass before any deletion.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC
    lux::cxx::expected<WorldSourceGarbageCollectionResult, std::string>
    collectWorldSourceGarbage(
        const std::filesystem::path& root_document,
        const WorldSourceGarbageCollectionConfig& config = {},
        const WorldSourceCodecLimits& limits = {}) noexcept;
} // namespace lux::authoring
