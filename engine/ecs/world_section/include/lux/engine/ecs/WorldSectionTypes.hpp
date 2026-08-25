#pragma once

#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/ecs/WorldSectionId.hpp>

#include <cstddef>
#include <cstdint>

namespace lux::ecs
{
    enum class EWorldSectionValueEncoding : std::uint8_t
    {
        TAG,
        FIXED,
        VARIABLE,
    };

    enum class EWorldSectionOrdinalEncoding : std::uint8_t
    {
        DENSE,
        U32_LIST,
    };

    enum class EWorldSectionError : std::uint8_t
    {
        WORLD_BUSY,
        INVALID_MAGIC,
        UNSUPPORTED_FORMAT_VERSION,
        UNSUPPORTED_LOADER_CONTRACT,
        INVALID_HEADER,
        INVALID_SECTION_ID,
        INVALID_SCHEMA,
        INVALID_COLUMN,
        INVALID_ENCODING,
        INVALID_ORDINAL,
        INVALID_OFFSETS,
        INVALID_PAYLOAD,
        OVERLAPPING_REGION,
        TRUNCATED,
        LIMIT_EXCEEDED,
        MISSING_BINDING,
        DUPLICATE_BINDING,
        BINDING_MISMATCH,
        DECODE_FAILED,
        ALLOCATION_FAILURE,
        WRONG_WORLD,
        INVALID_INSTANCE_STATE,
    };

    struct WorldSectionFailure final
    {
        EWorldSectionError code{EWorldSectionError::ALLOCATION_FAILURE};
        std::uint64_t byte_offset{};
        std::uint32_t column_index{};
        ComponentSchemaId schema;
    };

    struct WorldSectionLimits final
    {
        std::uint32_t max_entities{16U * 1024U * 1024U};
        std::uint32_t max_columns{4096U};
        std::uint64_t max_name_bytes{64ULL * 1024ULL * 1024ULL};
        std::uint64_t max_ordinal_bytes{256ULL * 1024ULL * 1024ULL};
        std::uint64_t max_offset_bytes{256ULL * 1024ULL * 1024ULL};
        std::uint64_t max_payload_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL - 1ULL};
        std::uint64_t max_image_bytes{8ULL * 1024ULL * 1024ULL * 1024ULL};
    };
} // namespace lux::ecs
