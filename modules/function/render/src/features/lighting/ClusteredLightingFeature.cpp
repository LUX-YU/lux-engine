#include <lux/engine/render/features/lighting/ClusteredLightingFeature.hpp>

#include "ClusterBuild.hpp"
#include "ClusterCount.hpp"
#include "ClusterFill.hpp"
#include "ClusterScan.hpp"
#include <cstring>
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/resource/view/CameraParameters.hpp>
#include <lux/engine/render/features/lighting/LightPorts.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>

namespace lux::render
{
    RenderResult<void>
    detail::ClusteredLightingFeature::Ports::describe(
        const Config* value,
        std::string_view,
        FeaturePorts& ports
    ) noexcept
    {
        const auto& config = *static_cast<const ClusteredLightingFeatureConfig*>(value);
        const bool missing_storage = !config.external_lights && config.lights.empty();
        if (!config.grid.valid() || missing_storage)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        describeLightPorts(ports, config.lights, config.external_lights);
        const auto output = [&](std::string key, std::uint64_t size, std::string_view semantic = {})
        {
            FeaturePort port{std::move(key), EFeaturePort::OUTPUT, GraphBufferDesc{size}};
            port.semantic = semantic;
            ports.resources.push_back(std::move(port));
        };
        const auto count = config.grid.count();
        output("bounds", count * sizeof(ClusterBoundsGpu));
        output("counts", count * 4);
        output("grid", sizeof(ClusterGridGpu), kClusterGrid);
        output("offsets", (count + 1) * 4, kClusterOffsets);
        output("indices", std::uint64_t(config.grid.index_capacity) * 4, kClusterIndices);
        output("status", sizeof(ClusterStatusGpu), kClusterStatus);
        return {};
    }

    RenderResult<void>
    detail::ClusteredLightingFeature::build(
        const Config* value,
        std::string_view key,
        FeatureBuildContext& context
    ) noexcept
    {
        const auto& config = *static_cast<const ClusteredLightingFeatureConfig*>(value);
        const auto count = static_cast<std::uint32_t>(config.grid.count());
        const auto dispatch = (count / 64) + (count % 64 != 0);
        if (!config.external_lights) requireLightPorts(context);
        auto build_program = context.graph.addProgram(GraphProgram{kClusterBuild});
        auto count_program = context.graph.addProgram(GraphProgram{kClusterCount});
        auto scan_program = context.graph.addProgram(GraphProgram{kClusterScan});
        auto fill_program = context.graph.addProgram(GraphProgram{kClusterFill});
        if (!build_program || !count_program || !scan_program || !fill_program)
        {
            return cxx::unexpected(
                !build_program   ? build_program.error()
                : !count_program ? count_program.error()
                : !scan_program  ? scan_program.error()
                                 : fill_program.error()
            );
        }
        auto reset = context.graph.addTransferPass(
            featureKey(detail::ClusteredLightingFeature::name, key, "reset"),
            TransferWork{{FillBuffer{{context.resource("status")}, 0}}}
        );
        if (!reset)
        {
            return cxx::unexpected(reset.error());
        }
        auto status = reset->written[0];
        // Fill writes every referenced index; offsets/counts bound all consumers, including overflow fallback.
        const auto indices = context.resource("indices");
        ComputeWork build;
        build.interface.program = *build_program;
        build.interface.resources = {
            {"bounds", {context.resource("bounds"), {}, EGraphAccess::WRITE}},
            {"grid_data", {context.resource("grid"), {}, EGraphAccess::WRITE}},
            {"status", {status, {}, EGraphAccess::READ_WRITE}}
        };
        build.interface.parameters.source = EParameterSource::VIEW;
        build.interface.parameters.instance_layout = cameraParameterLayout();
        build.interface.parameters.values.resize(128);
        const std::array<std::uint32_t, 8> constants{
            config.grid.cells[0],
            config.grid.cells[1],
            config.grid.cells[2],
            static_cast<std::uint32_t>(config.grid.depth),
            count,
            config.grid.index_capacity,
            0,
            0
        };
        std::memcpy(build.interface.parameters.values.data() + 96, constants.data(), sizeof(constants));
        build.interface.parameters.fields = {
            {"inverse_view_projection", {}, "inverse_view_projection"},
            {"position_near", {}, "position_near"},
            {"direction_far", {}, "direction_far"}
        };
        build.dispatch = DispatchArguments{{dispatch, 1, 1}};
        build.allow_async_compute = true;
        auto built = context.graph.addComputePass(
            featureKey(detail::ClusteredLightingFeature::name, key, "build"),
            std::move(build)
        );
        if (!built)
        {
            return cxx::unexpected(built.error());
        }
        const auto bounds = built->written[0], grid = built->written[1];
        status = built->written[2];
        const auto lights = [&](PassInterface& shader)
        {
            for (auto name : {"point_lights", "spot_lights", "light_counts"})
            {
                shader.resources.push_back({name, {context.resource(name)}});
            }
        };
        ComputeWork counting;
        counting.interface.program = *count_program;
        counting.interface.resources = {
            {"bounds", {bounds}},
            {"counts", {context.resource("counts"), {}, EGraphAccess::WRITE}}
        };
        lights(counting.interface);
        counting.dispatch = DispatchArguments{{dispatch, 1, 1}};
        counting.allow_async_compute = true;
        auto counted = context.graph.addComputePass(
            featureKey(detail::ClusteredLightingFeature::name, key, "count"),
            std::move(counting)
        );
        if (!counted)
        {
            return cxx::unexpected(counted.error());
        }
        const auto counts = counted->written[0];
        ComputeWork scan;
        scan.interface.program = *scan_program;
        scan.interface.resources = {
            {"counts", {counts}},
            {"offsets", {context.resource("offsets"), {}, EGraphAccess::WRITE}},
            {"status", {status, {}, EGraphAccess::READ_WRITE}}
        };
        const std::array scan_parameters{count, config.grid.index_capacity};
        const auto bytes = std::as_bytes(std::span(scan_parameters));
        scan.interface.parameters.values.assign(bytes.begin(), bytes.end());
        scan.dispatch = DispatchArguments{{1, 1, 1}};
        scan.allow_async_compute = true;
        auto scanned = context.graph.addComputePass(
            featureKey(detail::ClusteredLightingFeature::name, key, "scan"),
            std::move(scan)
        );
        if (!scanned)
        {
            return cxx::unexpected(scanned.error());
        }
        const auto offsets = scanned->written[0];
        status = scanned->written[1];
        ComputeWork fill;
        fill.interface.program = *fill_program;
        fill.interface.resources = {
            {"bounds", {bounds}},
            {"offsets", {offsets}},
            {"status", {status}},
            {"indices", {indices, {}, EGraphAccess::WRITE}}
        };
        lights(fill.interface);
        fill.dispatch = DispatchArguments{{dispatch, 1, 1}};
        fill.allow_async_compute = true;
        auto filled = context.graph.addComputePass(
            featureKey(detail::ClusteredLightingFeature::name, key, "fill"),
            std::move(fill)
        );
        if (!filled)
        {
            return cxx::unexpected(filled.error());
        }
        const std::array outputs{
            std::pair{"bounds", bounds},
            std::pair{"counts", counts},
            std::pair{"grid", grid},
            std::pair{"offsets", offsets},
            std::pair{"indices", filled->written[0]},
            std::pair{"status", status}
        };
        for (const auto& [name, version] : outputs)
        {
            if (auto published = context.publish(name, version); !published)
            {
                return published;
            }
        }
        return {};
    }

    RenderResult<std::size_t> ConfigValueTraits<ClusteredLightingFeatureConfig>::retainedBytes(
        const ClusteredLightingFeatureConfig& value
    ) noexcept
    {
        auto bytes = sizeof(value);
        const auto data = reinterpret_cast<std::uintptr_t>(value.lights.data());
        const auto begin = reinterpret_cast<std::uintptr_t>(&value.lights);
        if (data < begin || data >= begin + sizeof(value.lights))
        {
            bytes += value.lights.capacity() + 1;
        }
        return bytes;
    }

} // namespace lux::render
