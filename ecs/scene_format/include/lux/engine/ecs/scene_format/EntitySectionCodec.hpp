#pragma once
/**
 * @file EntitySectionCodec.hpp
 * @brief Deterministic, bounded LXES v1 codec surface.
 */

#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/ecs/scene_format/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lux::ecs::scene_format
{
    struct EntitySectionCodecLimits final
    {
        std::uint64_t maximum_section_bytes{1024ull * 1024ull * 1024ull};
        std::uint64_t maximum_decode_allocation_bytes{
            1536ull * 1024ull * 1024ull};
        std::uint32_t maximum_string_bytes{4096u};
        std::uint32_t maximum_names{1u << 20u};
        std::uint32_t maximum_entities_per_section{4u * 1024u * 1024u};
        std::uint32_t maximum_schemas_per_section{65536u};
        std::uint32_t maximum_archetypes_per_section{65536u};
        std::uint32_t maximum_columns_per_section{1u << 20u};
        std::uint32_t maximum_relocations_per_section{16u << 20u};
        std::uint32_t maximum_attachments_per_section{1u << 20u};
        std::uint32_t maximum_blob_relocations_per_section{16u << 20u};
    };

    enum class EEntitySectionCodecError : std::uint8_t
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

    struct EntitySectionCodecFailure final
    {
        EEntitySectionCodecError error{
            EEntitySectionCodecError::INVALID_ARGUMENT};
        std::string detail;
    };

    template <typename T>
    using EntitySectionCodecResult =
        lux::cxx::expected<T, EntitySectionCodecFailure>;

    [[nodiscard]] LUX_ECS_SCENE_FORMAT_PUBLIC
    lux::cxx::algorithm::Sha256Digest entitySectionContentDigest(
        std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] LUX_ECS_SCENE_FORMAT_PUBLIC ContentBlobId makeContentBlobId(
        const ContentTypeId& type,
        std::uint32_t schema_version,
        std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] LUX_ECS_SCENE_FORMAT_PUBLIC
    EntitySectionCodecResult<void> validateEntitySectionImage(
        const EntitySectionImage& image,
        const EntitySectionCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ECS_SCENE_FORMAT_PUBLIC
    EntitySectionCodecResult<std::vector<std::byte>> encodeEntitySectionImage(
        const EntitySectionImage& image,
        const EntitySectionCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ECS_SCENE_FORMAT_PUBLIC
    EntitySectionCodecResult<EntitySectionImage> decodeEntitySectionImage(
        std::span<const std::byte> bytes,
        const EntitySectionCodecLimits& limits = {}) noexcept;
} // namespace lux::ecs::scene_format
