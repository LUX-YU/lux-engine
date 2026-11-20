#pragma once

#include <cstdint>
#include <lux/engine/function/graph/GraphTypes.hpp>

namespace lux::render
{
    enum class EEffectIssue : std::uint8_t
    {
        CATALOG_NOT_SEALED,
        CAPACITY,
        MISSING_NODE_PAYLOAD,
        UNKNOWN_CAPABILITY,
        UNKNOWN_IMPLEMENTATION,
        SCHEMA_MISMATCH,
        INSTANCE_LIMIT,
        INVALID_LINK,
        TYPE_MISMATCH,
        FAN_LIMIT,
        CYCLE,
        INVALID_OUTPUT,
        MISSING_INPUT,
        UNSUPPORTED_OUTPUT,
        INITIALIZATION_FAILURE,
        BUILD_FAILURE
    };

    // Source identity and numeric detail remain valid after the graph and the callback have ended.
    struct EffectDiagnostic final
    {
        EEffectIssue issue;
        graph::NodeId node;
        graph::PinId pin;
        graph::PinId related_pin;
        std::uint32_t detail = 0;
    };
} // namespace lux::render
