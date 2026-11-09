#pragma once

#include <lux/engine/ecs/ComponentSchemaId.hpp>

#include <cstdint>

namespace lux::ecs
{
    enum class ESnapshotError : std::uint8_t
    {
        STATE_BUSY,
        UNKNOWN_COMPONENT_STORAGE,
        INVALID_COPY_SCHEMA,
        DUPLICATE_BINDING,
        BINDING_MISMATCH,
    };

    struct SnapshotError final
    {
        ESnapshotError code{ESnapshotError::INVALID_COPY_SCHEMA};
        std::uint64_t storage_id{};
        ComponentSchemaId schema;
    };
} // namespace lux::ecs
