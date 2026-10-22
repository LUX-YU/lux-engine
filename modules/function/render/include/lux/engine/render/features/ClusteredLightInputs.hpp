#pragma once

#include <array>
#include <cstdint>
#include <lux/engine/description/ShaderBlock.hpp>
#include <string_view>

namespace lux::render
{
    enum class EClusterDepth : std::uint32_t
    {
        LINEAR,
        LOGARITHMIC
    };

    struct ClusterGrid final
    {
        std::array<std::uint32_t, 3> cells{16, 9, 24};
        std::uint32_t index_capacity = 65536;
        EClusterDepth depth = EClusterDepth::LOGARITHMIC;
        bool operator==(const ClusterGrid&) const noexcept = default;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            const bool invalid_capacity = !index_capacity || index_capacity == UINT32_MAX;
            if (invalid_capacity || depth > EClusterDepth::LOGARITHMIC)
            {
                return false;
            }
            std::uint32_t size = 1;
            for (auto dimension : cells)
            {
                if (!dimension || size > (UINT32_MAX - 1) / dimension)
                {
                    return false;
                }
                size *= dimension;
            }
            return true;
        }

        [[nodiscard]] constexpr std::uint64_t count() const noexcept
        {
            return std::uint64_t(cells[0]) * cells[1] * cells[2];
        }
    };

    // Finite forward-Z projection. Both perspective and orthographic use its actual inverse;
    // depth slices are distances along the supplied unit forward vector, not clip-space Z.
    struct ClusteredLightingParameters final
    {
        std::array<float, 16> inverse_view_projection{};
        std::array<float, 4> position_near{0, 0, 0, 0.1F};
        std::array<float, 4> direction_far{0, 0, -1, 100};
    };

    [[nodiscard]] inline rdesc::ShaderBlock clusteredLightingParameterBlock() noexcept
    {
        rdesc::ShaderBlock block{sizeof(ClusteredLightingParameters), 16};
        block.fields = {
            {"cluster_inverse_view_projection", rdesc::EDataScalar::FLOAT, 0, 4, 4, 0, 16},
            {"cluster_position_near", rdesc::EDataScalar::FLOAT, 64, 4, 1, 0, 0},
            {"cluster_direction_far", rdesc::EDataScalar::FLOAT, 80, 4, 1, 0, 0}
        };
        return block;
    }

    struct ClusterBoundsGpu final
    {
        std::array<float, 4> minimum{}, maximum{};
    };

    struct ClusterGridGpu final
    {
        std::array<std::uint32_t, 4> cells{};  // xyz dimensions, w EClusterDepth.
        std::array<std::uint32_t, 4> limits{}; // cluster count, index capacity, reserved.
        std::array<float, 4> position_near{}, direction_far{};
        std::array<float, 16> world_to_clip{};
    };

    struct ClusterStatusGpu final
    {
        std::uint32_t required_indices = 0; // Saturates at UINT32_MAX; never wraps.
        std::uint32_t flags = 0;            // 1: capacity, 2: invalid projection, 4: saturated sum.
        std::uint32_t reserved[2]{};
    };

    static_assert(sizeof(ClusterBoundsGpu) == 32 && sizeof(ClusterGridGpu) == 128);
    static_assert(sizeof(ClusterStatusGpu) == 16 && sizeof(ClusteredLightingParameters) == 96);
    inline constexpr std::string_view kClusterGrid = "lux.light.cluster.grid.forward-z.v1";
    inline constexpr std::string_view kClusterOffsets = "lux.light.cluster.exclusive-offsets.v1";
    // Legacy packed index: low 31 bits dense index, bit 31 selects spot instead of point.
    inline constexpr std::string_view kClusterIndices = "lux.light.cluster.point-spot-indices.v1";
    inline constexpr std::string_view kClusterStatus = "lux.light.cluster.status.v1";
} // namespace lux::render
