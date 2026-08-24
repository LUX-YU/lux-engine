#pragma once

#include <lux/engine/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/ecs/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/WorldSectionImage.hpp>
#include <lux/engine/ecs/persistence/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lux::ecs
{
    enum class EPersistenceError : std::uint8_t
    {
        WORLD_BUSY,
        INVALID_ENTITY,
        MISSING_PERSISTENT_ID,
        DUPLICATE_PERSISTENT_ID,
        DUPLICATE_ENTITY,
        MISSING_SCHEMA,
        MISSING_CODEC,
        COMPONENT_ENCODE_FAILED,
        COMPONENT_DECODE_FAILED,
        ENTITY_REFERENCE_OUTSIDE_SECTION,
        INVALID_MAGIC,
        UNSUPPORTED_VERSION,
        TRUNCATED,
        INVALID_INDEX,
        INVALID_HASH,
        INVALID_SECTION_ID,
        INVALID_NAME_TABLE,
        INVALID_SCHEMA_VERSION,
        DUPLICATE_SCHEMA,
        INVALID_ARCHETYPE,
        DUPLICATE_COMPONENT,
        INVALID_RELOCATION,
        INVALID_PAYLOAD,
        LIMIT_EXCEEDED,
        ALLOCATION_FAILURE,
    };

    struct PersistenceFailure final
    {
        EPersistenceError code{EPersistenceError::ALLOCATION_FAILURE};
        std::uint32_t entity_ordinal{};
        ComponentSchemaId schema;
    };

    struct WorldSectionWriteSelection final
    {
        std::span<const Entity> entities;
        std::span<const ComponentSchemaId> schemas;
    };

    struct WorldSectionLimits final
    {
        std::uint32_t max_names{65536};
        std::uint32_t max_schemas{4096};
        std::uint32_t max_archetypes{65536};
        std::uint32_t max_entities{16U * 1024U * 1024U};
        std::uint32_t max_columns{4096};
        std::uint32_t max_cells{16U * 1024U * 1024U};
        std::uint32_t max_relocations{16U * 1024U * 1024U};
        std::uint64_t max_name_bytes{64ULL * 1024ULL * 1024ULL};
        std::uint64_t max_payload_bytes{1ULL * 1024ULL * 1024ULL * 1024ULL};
        std::uint64_t max_image_bytes{2ULL * 1024ULL * 1024ULL * 1024ULL};
    };

    class LUX_ENGINE_ECS_PERSISTENCE_PUBLIC WorldSectionWriter final
    {
      public:
        [[nodiscard]] static lux::cxx::expected<WorldSectionImage, PersistenceFailure>
        build(
            const World& world,
            const ComponentSchemaSet& schemas,
            WorldSectionId id,
            WorldSectionWriteSelection selection
        ) noexcept;
    };

    class LUX_ENGINE_ECS_PERSISTENCE_PUBLIC WorldSectionReader final
    {
      public:
        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<World>, PersistenceFailure>
        materialize(
            const WorldSectionImage& image,
            const ComponentSchemaSet& schemas,
            WorldConfig config = {}
        ) noexcept;
    };

    [[nodiscard]] LUX_ENGINE_ECS_PERSISTENCE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, PersistenceFailure>
    encodeWorldSection(const WorldSectionImage& image) noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_PERSISTENCE_PUBLIC
    lux::cxx::expected<WorldSectionImage, PersistenceFailure>
    decodeWorldSection(
        std::span<const std::byte> bytes,
        WorldSectionLimits limits = {}) noexcept;
} // namespace lux::ecs
