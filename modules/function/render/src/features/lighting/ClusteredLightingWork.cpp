#include "ClusterBuild.hpp"
#include "ClusterCount.hpp"
#include "ClusterFill.hpp"
#include "ClusterScan.hpp"
#include <lux/engine/render/effect/Lighting.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/features/lighting/ClusteredLightingWork.hpp>

namespace lux::render::detail
{
    struct ClusterScanConstants final
    {
        std::uint32_t cluster_count;
        std::uint32_t capacity;
    };
}

namespace lux::render
{
    template <> struct ConstantBlockTraits<detail::ClusterScanConstants> final
    {
        static ConstantBlockLayout layout(rdesc::EShaderTarget target) noexcept
        {
            using Block = detail::ClusterScanConstants;
            static_assert(sizeof(Block) == 8);
            static constexpr std::array members{
                ConstantMemberLayout{"cluster_count", offsetof(Block, cluster_count), rdesc::EDataScalar::UINT},
                ConstantMemberLayout{"capacity", offsetof(Block, capacity), rdesc::EDataScalar::UINT}
            };
            const auto alignment = target == rdesc::EShaderTarget::DXIL ? 16u : static_cast<std::uint32_t>(alignof(Block));
            return {sizeof(Block), alignment, members};
        }
    };
}

namespace lux::render::detail
{
    RenderResult<ClusterBuildResult> buildClusterLightLists(
        EffectCompileContext& builder, std::string_view key, const ClusterGrid& grid,
        const std::array<ResourceVersion, 4>& light_bindings
    ) noexcept
    {
        ClusterBuffers initial;
        const auto sizes = clusterBufferSizes(grid);
        for (std::size_t i = 0; i < clusterBindings.size(); ++i)
        {
            const auto& [name, field] = clusterBindings[i];
            auto resource = builder.declareResource({
                std::string{key} + "/" + std::string{name}, GraphBufferDesc{sizes[i]}
            });
            if (!resource) return cxx::unexpected(resource.error());
            initial.*field = {*resource, 0};
        }
        const auto count = static_cast<std::uint32_t>(grid.count());
        const auto dispatch = (count / 64) + (count % 64 != 0);
        const auto prefix = std::string{key} + "/";
        const auto build_program = builder.addProgram(GraphProgram{kClusterBuild});
        if (!build_program) return cxx::unexpected(build_program.error());
        const auto count_program = builder.addProgram(GraphProgram{kClusterCount});
        if (!count_program) return cxx::unexpected(count_program.error());
        const auto scan_program = builder.addProgram(GraphProgram{kClusterScan});
        if (!scan_program) return cxx::unexpected(scan_program.error());
        const auto fill_program = builder.addProgram(GraphProgram{kClusterFill});
        if (!fill_program) return cxx::unexpected(fill_program.error());

        auto reset = builder.addTransferPass(prefix + "reset", TransferWork{{FillBuffer{{initial.status}, 0}}});
        if (!reset) return cxx::unexpected(reset.error());
        auto status = reset->written[0];
        ComputeWork build;
        build.interface.program = *build_program;
        build.interface.resources = {
            {"bounds", {initial.bounds, {}, EGraphAccess::WRITE}},
            {"grid_data", {initial.grid, {}, EGraphAccess::WRITE}},
            {"status", {status, {}, EGraphAccess::READ_WRITE}}
        };
        build.interface.parameters.setInlineConstant(clusterBuildConstants(grid));
        build.dispatch = DispatchArguments{{dispatch, 1, 1}};
        build.allow_async_compute = true;
        auto built = builder.addComputePass(prefix + "build", std::move(build));
        if (!built) return cxx::unexpected(built.error());
        const auto bounds = built->written[0], grid_data = built->written[1];
        status = built->written[2];

        const auto bind_lights = [&](PassInterface& shader)
        {
            for (std::size_t i = 1; i < light_bindings.size(); ++i)
                shader.resources.push_back({std::string{Lighting::light_bindings[i]}, {light_bindings[i]}});
        };
        ComputeWork counting;
        counting.interface.program = *count_program;
        counting.interface.resources = {
            {"bounds", {bounds}},
            {"counts", {initial.counts, {}, EGraphAccess::WRITE}}
        };
        bind_lights(counting.interface);
        counting.dispatch = DispatchArguments{{dispatch, 1, 1}};
        counting.allow_async_compute = true;
        auto counted = builder.addComputePass(prefix + "count", std::move(counting));
        if (!counted) return cxx::unexpected(counted.error());
        const auto counts = counted->written[0];

        ComputeWork scan;
        scan.interface.program = *scan_program;
        scan.interface.resources = {
            {"counts", {counts}},
            {"offsets", {initial.offsets, {}, EGraphAccess::WRITE}},
            {"status", {status, {}, EGraphAccess::READ_WRITE}}
        };
        scan.interface.parameters.setInlineConstant(ClusterScanConstants{count, grid.index_capacity});
        scan.dispatch = DispatchArguments{{1, 1, 1}};
        scan.allow_async_compute = true;
        auto scanned = builder.addComputePass(prefix + "scan", std::move(scan));
        if (!scanned) return cxx::unexpected(scanned.error());
        const auto offsets = scanned->written[0];
        status = scanned->written[1];

        // Offsets and status bound every consumer, including suppressed writes on overflow.
        ComputeWork fill;
        fill.interface.program = *fill_program;
        fill.interface.resources = {
            {"bounds", {bounds}},
            {"offsets", {offsets}},
            {"status", {status}},
            {"indices", {initial.indices, {}, EGraphAccess::WRITE}}
        };
        bind_lights(fill.interface);
        fill.dispatch = DispatchArguments{{dispatch, 1, 1}};
        fill.allow_async_compute = true;
        auto filled = builder.addComputePass(prefix + "fill", std::move(fill));
        if (!filled) return cxx::unexpected(filled.error());
        return ClusterBuildResult{built->pass, {bounds, counts, grid_data, offsets, filled->written[0], status}};
    }
}
