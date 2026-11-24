#pragma once

#include <lux/engine/render/graph/Work.hpp>
#include <lux/engine/render/effect/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/effect/resources/ClusterLists.hpp>
#include <lux/engine/render/resource/transform/Data.hpp>
#include <utility>

namespace lux::render
{
    class EffectCompileContext;
}

namespace lux::render::detail
{
    struct DeferredLightingInputs;

    struct alignas(16) ClusterBuildConstants final
    {
        std::array<std::uint32_t, 4> cells, limits;
    };

    struct ClusterBuffers final
    {
        ResourceVersion bounds, counts, grid, offsets, indices, status;
    };

    inline constexpr std::array clusterBindings{
        std::pair{ClusterListBundle::bounds, &ClusterBuffers::bounds},
        std::pair{ClusterListBundle::counts, &ClusterBuffers::counts},
        std::pair{ClusterListBundle::grid, &ClusterBuffers::grid},
        std::pair{ClusterListBundle::offsets, &ClusterBuffers::offsets},
        std::pair{ClusterListBundle::indices, &ClusterBuffers::indices},
        std::pair{ClusterListBundle::status, &ClusterBuffers::status}
    };

    [[nodiscard]] constexpr std::array<std::uint64_t, 6> clusterBufferSizes(const ClusterGrid& grid) noexcept
    {
        const auto count = grid.count();
        return {
            count * sizeof(ClusterBoundsGpu), count * 4, sizeof(ClusterGridGpu),
            (count + 1) * 4, std::uint64_t(grid.index_capacity) * 4, sizeof(ClusterStatusGpu)
        };
    }

    [[nodiscard]] constexpr ClusterBuildConstants clusterBuildConstants(const ClusterGrid& grid) noexcept
    {
        return {
            {grid.cells[0], grid.cells[1], grid.cells[2], static_cast<std::uint32_t>(grid.depth)},
            {static_cast<std::uint32_t>(grid.count()), grid.index_capacity, 0, 0}
        };
    }

    [[nodiscard]] RenderResult<ClusterBuffers> buildClusterLightLists(
        EffectCompileContext&, std::string_view key, const ClusterGrid&,
        const DeferredLightingInputs& inputs
    ) noexcept;
}

namespace lux::render
{
    template <> struct ConstantBlockTraits<detail::ClusterBuildConstants> final
    {
        static ConstantBlockLayout layout(rdesc::EShaderTarget) noexcept
        {
            using Block = detail::ClusterBuildConstants;
            constexpr auto u32 = rdesc::EDataScalar::UINT;
            static constexpr std::array members{
                ConstantMemberLayout{"cells", offsetof(Block, cells), u32, 4},
                ConstantMemberLayout{"limits", offsetof(Block, limits), u32, 4}
            };
            return {sizeof(Block), alignof(Block), members};
        }
    };
}
