#pragma once
/**
 * @file StaticColliderBatch3D.hpp
 * @brief Description-owned immutable 3D static-collider values.
 */

#include <cstdint>
#include <vector>

namespace lux::physics3d
{
    /// Entity-local offset composed with the resolved transform by Runtime.
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
} // namespace lux::physics3d
