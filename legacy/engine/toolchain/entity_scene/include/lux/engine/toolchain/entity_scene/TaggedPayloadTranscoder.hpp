#pragma once
/**
 * @file TaggedPayloadTranscoder.hpp
 * @brief Re-index tagged-property payloads into one canonical shared NameTable.
 */

#include <lux/engine/toolchain/entity_scene/EntitySceneCookError.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace lux::toolchain
{
    /// One tagged-property object together with the NameTable whose indices
    /// are embedded in its payload. Index zero must be the empty sentinel.
    struct TaggedPayloadSource final
    {
        std::vector<std::string> names{std::string{}};
        std::vector<std::byte> payload;
    };

    /// Decode the standalone NameTable bytes used by legacy authoring
    /// documents into an owning, index-preserving list. This helper does not
    /// accept trailing bytes, empty non-zero entries, or duplicate names.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<std::vector<std::string>, EntitySceneCookFailure>
    decodeTaggedPayloadNameTable(std::span<const std::byte> image) noexcept;

    /// Scan one or more complete tagged-property objects and return the names
    /// they actually reference. The result has an empty index-zero sentinel;
    /// all remaining names are unique and bytewise sorted.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<std::vector<std::string>, EntitySceneCookFailure>
    canonicalTaggedPayloadNames(
        std::span<const TaggedPayloadSource> payloads) noexcept;

    /// Rewrite every top-level and nested name index in @p source to the
    /// matching index in @p canonical_names. The destination table must have
    /// the canonical shape returned by canonicalTaggedPayloadNames().
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, EntitySceneCookFailure>
    transcodeTaggedPayloadNames(
        const TaggedPayloadSource& source,
        std::span<const std::string> canonical_names) noexcept;
}
