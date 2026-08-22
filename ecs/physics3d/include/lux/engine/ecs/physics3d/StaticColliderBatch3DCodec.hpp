#pragma once
/**
 * @file StaticColliderBatch3DCodec.hpp
 * @brief LXPC v1 static-collider batch wire contract.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/physics3d/StaticColliderBatch3D.hpp>
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::physics3d
{
    inline constexpr std::string_view kStaticColliderBatch3DContentTypeName =
        "lux.physics3d.static-collider-batch";
    inline constexpr std::uint32_t kStaticColliderBatch3DSchemaVersion = 1u;
    inline constexpr std::uint32_t kStaticColliderBatch3DBlobMagic =
        0x4350584cu; // LXPC

    inline constexpr std::uint32_t
        kStaticColliderBatch3DMaximumHeightfields = 4096u;
    inline constexpr std::uint32_t
        kStaticColliderBatch3DMaximumSampleEdge = 4097u;
    inline constexpr std::uint64_t
        kStaticColliderBatch3DMaximumSamples = 16u * 1024u * 1024u;

    enum class EStaticColliderBatch3DCodecError : std::uint8_t
    {
        INVALID_ARGUMENT,
        BAD_MAGIC,
        UNSUPPORTED_VERSION,
        TRUNCATED,
        INVALID_LAYOUT,
        INVALID_VALUE,
        LIMIT_EXCEEDED,
        TRAILING_BYTES
    };

    struct StaticColliderBatch3DCodecFailure final
    {
        EStaticColliderBatch3DCodecError error{
            EStaticColliderBatch3DCodecError::INVALID_ARGUMENT};
        std::string detail;
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC
    lux::cxx::expected<void, StaticColliderBatch3DCodecFailure>
    validateStaticColliderBatch3DBlob(
        const StaticColliderBatch3DBlobV1& blob) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC
    lux::cxx::expected<
        std::vector<std::byte>,
        StaticColliderBatch3DCodecFailure>
    encodeStaticColliderBatch3DBlob(
        const StaticColliderBatch3DBlobV1& blob) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC
    lux::cxx::expected<
        StaticColliderBatch3DBlobV1,
        StaticColliderBatch3DCodecFailure>
    decodeStaticColliderBatch3DBlob(
        std::span<const std::byte> bytes) noexcept;
} // namespace lux::physics3d
