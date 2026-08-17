#pragma once
/**
 * @file StaticColliderBatch3D.hpp
 * @brief Domain-owned cooked static-collider content for 3D physics.
 *
 * The blob contains immutable collision geometry only.  Entity placement,
 * Section identity, residency and backend handles remain outside this wire
 * contract.  A runtime leaf resolves the owning entity transform and prepares
 * the private Jolt representation.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/physics3d/visibility.h>
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

    /// Plain entity-local offset.  Unlike spatial::Position3D this value is
    /// not an absolute registry-space position; the runtime adapter composes
    /// it with the entity's resolved transform before physics preparation.
    struct StaticColliderLocalOffset3D final
    {
        double x{0.0};
        double y{0.0};
        double z{0.0};

        friend bool operator==(
            const StaticColliderLocalOffset3D&,
            const StaticColliderLocalOffset3D&) = default;
    };

    struct StaticHeightfieldCollider3DV1 final
    {
        /// Local to the entity carrying StaticColliderBatch3DComponent.
        StaticColliderLocalOffset3D local_origin;
        std::uint32_t sample_edge{0u};
        float sample_spacing{1.0f};
        float height_min{0.0f};
        float height_max{1.0f};
        std::vector<std::uint16_t> samples;

        friend bool operator==(
            const StaticHeightfieldCollider3DV1&,
            const StaticHeightfieldCollider3DV1&) = default;
    };

    struct StaticColliderBatch3DBlobV1 final
    {
        std::vector<StaticHeightfieldCollider3DV1> heightfields;

        friend bool operator==(
            const StaticColliderBatch3DBlobV1&,
            const StaticColliderBatch3DBlobV1&) = default;
    };

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

    [[nodiscard]] LUX_ENGINE_RESOURCE_PHYSICS3D_PUBLIC
    lux::cxx::expected<void, StaticColliderBatch3DCodecFailure>
    validateStaticColliderBatch3DBlob(const StaticColliderBatch3DBlobV1& blob) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_PHYSICS3D_PUBLIC
    lux::cxx::expected<
        std::vector<std::byte>,
        StaticColliderBatch3DCodecFailure>
    encodeStaticColliderBatch3DBlob(const StaticColliderBatch3DBlobV1& blob) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_PHYSICS3D_PUBLIC
    lux::cxx::expected<
        StaticColliderBatch3DBlobV1,
        StaticColliderBatch3DCodecFailure>
    decodeStaticColliderBatch3DBlob(std::span<const std::byte> bytes) noexcept;
} // namespace lux::physics3d
