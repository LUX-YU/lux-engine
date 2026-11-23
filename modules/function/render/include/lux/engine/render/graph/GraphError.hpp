#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>
#include <span>

namespace lux::render
{
    // Error args: {pass index (or invalid), resource index (or invalid), detail}.
    inline constexpr auto kGraphUnknownResource = error::errorId("lux.render.graph.unknown_resource");
    inline constexpr auto kGraphUnknownPass     = error::errorId("lux.render.graph.unknown_pass");
    inline constexpr auto kGraphInvalidDescriptor = error::errorId("lux.render.graph.invalid_descriptor");
    inline constexpr auto kGraphInvalidRange    = error::errorId("lux.render.graph.invalid_range");
    inline constexpr auto kGraphDuplicateKey    = error::errorId("lux.render.graph.duplicate_key");
    inline constexpr auto kGraphConflictingOutput = error::errorId("lux.render.graph.conflicting_output");
    inline constexpr auto kGraphUndefinedRead   = error::errorId("lux.render.graph.undefined_read");
    inline constexpr auto kGraphAmbiguousProducer = error::errorId("lux.render.graph.ambiguous_producer");
    inline constexpr auto kGraphDependencyCycle = error::errorId("lux.render.graph.dependency_cycle");
    inline constexpr auto kGraphMissingFallback = error::errorId("lux.render.graph.missing_fallback");
    inline constexpr auto kGraphMissingInitial  = error::errorId("lux.render.graph.missing_initial");

    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> renderGraphErrorDescriptors() noexcept;
} // namespace lux::render
