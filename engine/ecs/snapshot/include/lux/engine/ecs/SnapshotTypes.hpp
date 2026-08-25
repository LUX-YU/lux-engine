#pragma once

#include <lux/engine/ecs/ComponentSchemaId.hpp>

#include <cstdint>

namespace lux::ecs
{
    enum class ESnapshotError : std::uint8_t
    {
        WORLD_BUSY,
        UNKNOWN_COMPONENT_STORAGE,
        INVALID_COPY_SCHEMA,
        DUPLICATE_BINDING,
        BINDING_MISMATCH,
        ALLOCATION_FAILURE,
    };

    struct SnapshotError final
    {
        ESnapshotError code{ESnapshotError::ALLOCATION_FAILURE};
        std::uint64_t storage_id{};
        ComponentSchemaId schema;
    };
} // namespace lux::ecs
