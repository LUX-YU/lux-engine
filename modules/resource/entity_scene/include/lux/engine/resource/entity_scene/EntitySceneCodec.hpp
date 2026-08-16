#pragma once
/**
 * @file EntitySceneCodec.hpp
 * @brief Deterministic LXSC v1 and LXES v1 codec surface.
 */

#include <lux/engine/resource/entity_scene/EntityScene.hpp>
#include <lux/engine/resource/entity_scene/EntitySection.hpp>
#include <lux/engine/resource/entity_scene/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lux::entity_scene
{
    struct EntitySceneCodecLimits final
    {
        std::uint64_t maximum_manifest_bytes{16u * 1024u * 1024u};
        std::uint64_t maximum_section_bytes{1024ull * 1024ull * 1024ull};
        // Decoder-owned containers are bounded separately from the wire
        // image.  A per-input amplification cap is applied as well, so a
        // tiny malformed image cannot request this entire allowance.
        std::uint64_t maximum_decode_allocation_bytes{
            1536ull * 1024ull * 1024ull};
        std::uint32_t maximum_string_bytes{4096u};
        std::uint32_t maximum_names{1u << 20u};
        std::uint32_t maximum_sections{4u * 1024u * 1024u};
        std::uint32_t maximum_dependencies_per_section{4096u};
        std::uint32_t maximum_requirements{65536u};
        std::uint32_t maximum_contributions{65536u};
        std::uint32_t maximum_generator_parameter_bytes{4u * 1024u * 1024u};
        std::uint32_t maximum_entities_per_section{4u * 1024u * 1024u};
        std::uint32_t maximum_schemas_per_section{65536u};
        std::uint32_t maximum_archetypes_per_section{65536u};
        std::uint32_t maximum_columns_per_section{1u << 20u};
        std::uint32_t maximum_relocations_per_section{16u << 20u};
        std::uint32_t maximum_attachments_per_section{1u << 20u};
        std::uint32_t maximum_blob_relocations_per_section{16u << 20u};
    };

    enum class EEntitySceneCodecError : std::uint8_t
    {
        INVALID_ARGUMENT,
        BAD_MAGIC,
        UNSUPPORTED_VERSION,
        TRUNCATED,
        LIMIT_EXCEEDED,
        INVALID_NAME,
        HASH_MISMATCH,
        DUPLICATE_ID,
        INVALID_REFERENCE,
        DIGEST_MISMATCH,
        TRAILING_BYTES
    };

    struct EntitySceneCodecFailure final
    {
        EEntitySceneCodecError error{
            EEntitySceneCodecError::INVALID_ARGUMENT};
        std::string detail;
    };

    [[nodiscard]] LUX_ENGINE_RESOURCE_ENTITY_SCENE_PUBLIC
    lux::cxx::algorithm::Sha256Digest entitySceneContentDigest(
        std::span<const std::byte> bytes) noexcept;

    // Blob identity includes the canonical content type and schema version as
    // well as the bytes, so identical byte strings with incompatible schemas
    // cannot alias in a SectionBlobStore.
    [[nodiscard]] LUX_ENGINE_RESOURCE_ENTITY_SCENE_PUBLIC ContentBlobId
    makeContentBlobId(
        const ContentTypeId& type,
        std::uint32_t schema_version,
        std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<void, EntitySceneCodecFailure>
    validateEntitySectionRecord(
        const EntitySectionRecord& record,
        const EntitySceneCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<void, EntitySceneCodecFailure>
    validateEntitySceneManifest(
        const EntitySceneManifest& manifest,
        const EntitySceneCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<void, EntitySceneCodecFailure>
    validateEntitySectionImage(
        const EntitySectionImage& image,
        const EntitySceneCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, EntitySceneCodecFailure>
    encodeEntitySceneManifest(
        const EntitySceneManifest& manifest,
        const EntitySceneCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<EntitySceneManifest, EntitySceneCodecFailure>
    decodeEntitySceneManifest(
        std::span<const std::byte> bytes,
        const EntitySceneCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, EntitySceneCodecFailure>
    encodeEntitySectionImage(
        const EntitySectionImage& image,
        const EntitySceneCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<EntitySectionImage, EntitySceneCodecFailure>
    decodeEntitySectionImage(
        std::span<const std::byte> bytes,
        const EntitySceneCodecLimits& limits = {}) noexcept;
}
