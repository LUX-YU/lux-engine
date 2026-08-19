#pragma once
/**
 * @file ScenePackageCodec.hpp
 * @brief Bounded deterministic LXSC v1 scene-package codec.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/scene/package/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lux::scene
{
    struct ScenePackageCodecLimits final
    {
        std::uint64_t maximum_manifest_bytes{16u * 1024u * 1024u};
        std::uint64_t maximum_section_bytes{1024ull * 1024ull * 1024ull};
        std::uint64_t maximum_decode_allocation_bytes{
            1536ull * 1024ull * 1024ull};
        std::uint32_t maximum_string_bytes{4096u};
        std::uint32_t maximum_names{1u << 20u};
        std::uint32_t maximum_sections{4u * 1024u * 1024u};
        std::uint32_t maximum_dependencies_per_section{4096u};
        std::uint32_t maximum_requirements{65536u};
        std::uint32_t maximum_features{65536u};
        std::uint32_t maximum_generator_parameter_bytes{4u * 1024u * 1024u};
        std::uint32_t maximum_entities_per_section{4u * 1024u * 1024u};
    };

    enum class ScenePackageCodecError : std::uint8_t
    {
        InvalidArgument,
        BadMagic,
        UnsupportedVersion,
        Truncated,
        LimitExceeded,
        InvalidName,
        HashMismatch,
        DuplicateId,
        InvalidReference,
        DigestMismatch,
        TrailingBytes
    };

    struct ScenePackageCodecFailure final
    {
        ScenePackageCodecError error{ScenePackageCodecError::InvalidArgument};
        std::string detail;
    };

    template <class T>
    using ScenePackageCodecResult =
        lux::cxx::expected<T, ScenePackageCodecFailure>;

    [[nodiscard]] LUX_ENGINE_SCENE_PACKAGE_PUBLIC
    ScenePackageCodecResult<void> validateScenePackage(
        const ScenePackage& package,
        const ScenePackageCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_PACKAGE_PUBLIC
    ScenePackageCodecResult<std::vector<std::byte>> encodeScenePackage(
        const ScenePackage& package,
        const ScenePackageCodecLimits& limits = {}) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_PACKAGE_PUBLIC
    ScenePackageCodecResult<ScenePackage> decodeScenePackage(
        std::span<const std::byte> bytes,
        const ScenePackageCodecLimits& limits = {}) noexcept;
} // namespace lux::scene
