#include <lux/engine/render/features/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/ParameterLayout.hpp>

#include "ClusterBuild.hpp"
#include "ClusterCount.hpp"
#include "ClusterFill.hpp"
#include "ClusterScan.hpp"
#include <cstring>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/storage/DirectLighting.hpp>

namespace lux::render
{
    namespace
    {
        constexpr std::array<std::string_view, 4>
            light_ports{"directional_lights", "point_lights", "spot_lights", "light_counts"};
        constexpr std::array<std::uint32_t, 4> light_strides{32, 48, 64, 16};

        RenderResult<void> describeClusters(const void* value, std::string_view, FeaturePorts& ports) noexcept
        {
            const auto& config = *static_cast<const ClusteredLightingFeatureConfig*>(value);
            const bool missing_storage = !config.external_lights && config.lights.empty();
            if (!config.grid.valid() || missing_storage)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            for (std::size_t i = 0; i < light_ports.size(); ++i)
            {
                if (config.external_lights)
                {
                    ports.resources.push_back(
                        {std::string(light_ports[i]), EFeaturePort::INPUT, GraphBufferDesc{light_strides[i]}}
                    );
                }
                else
                {
                    ports.storage.scene.push_back(
                        {std::string(light_ports[i]),
                         EFeaturePort::OUTPUT,
                         GraphBufferDesc{light_strides[i]},
                         EGraphOrigin::IMPORTED,
                         {GraphBufferRange{0, light_strides[i]}},
                         StorageRequirement{StorageName(kDirectLightingStorageType.name()), config.lights}}
                    );
                }
            }
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

        RenderResult<std::uint32_t> program(
            FeatureBuildContext& context,
            std::span<const std::uint32_t> code,
            std::span<const std::byte> reflection
        ) noexcept
        {
            rdesc::SpirvArtifact shader;
            shader.stages.push_back({rdesc::EShaderType::COMPUTE, "main", {code.begin(), code.end()}});
            if (!rdesc::ShaderInfo::deserialize(reflection, shader.interface))
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            return context.graph.addProgram(std::move(shader));
        }

        RenderResult<void> buildClusters(const void* value, std::string_view key, FeatureBuildContext& context) noexcept
        {
            const auto& config = *static_cast<const ClusteredLightingFeatureConfig*>(value);
            const auto count = static_cast<std::uint32_t>(config.grid.count());
            const auto dispatch = (count / 64) + (count % 64 != 0);
            for (std::uint32_t i = 0; i < light_ports.size() && !config.external_lights; ++i)
            {
                context.arguments.push_back(StorageArgument::make(
                    context.resource(light_ports[i]).resource,
                    DirectLightingBinding{static_cast<EDirectLightingBuffer>(i)}
                ));
            }
            auto build_program = program(context, kClusterBuild, kClusterBuildReflection);
            auto count_program = program(context, kClusterCount, kClusterCountReflection);
            auto scan_program = program(context, kClusterScan, kClusterScanReflection);
            auto fill_program = program(context, kClusterFill, kClusterFillReflection);
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
                featureKey(ClusteredLightingFeatureDescriptor::name, key, "reset"),
                TransferWork{{FillBuffer{{context.resource("status")}, 0}, FillBuffer{{context.resource("indices")}, 0}}
                }
            );
            if (!reset)
            {
                return cxx::unexpected(reset.error());
            }
            auto status = reset->written[0], indices = reset->written[1];
            ComputeWork build;
            build.interface.program = *build_program;
            build.interface.resources = {
                {"bounds", {context.resource("bounds"), {}, EGraphAccess::WRITE}},
                {"grid_data", {context.resource("grid"), {}, EGraphAccess::WRITE}},
                {"status", {status, {}, EGraphAccess::READ_WRITE}}
            };
            build.interface.parameters.instance_layout = config.input_layout;
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
                {"inverse_view_projection", {}, "cluster_inverse_view_projection"},
                {"position_near", {}, "cluster_position_near"},
                {"direction_far", {}, "cluster_direction_far"}
            };
            build.dispatch = DispatchArguments{{dispatch, 1, 1}};
            build.allow_async_compute = true;
            auto built = context.graph.addComputePass(
                featureKey(ClusteredLightingFeatureDescriptor::name, key, "build"),
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
                featureKey(ClusteredLightingFeatureDescriptor::name, key, "count"),
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
                featureKey(ClusteredLightingFeatureDescriptor::name, key, "scan"),
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
                featureKey(ClusteredLightingFeatureDescriptor::name, key, "fill"),
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
    } // namespace

    RenderResult<std::size_t> ConfigValueTraits<ClusteredLightingFeatureConfig>::retainedBytes(
        const ClusteredLightingFeatureConfig& value
    ) noexcept
    {
        auto bytes = sizeof(value) + detail::parameterLayoutBytes(value.input_layout);
        const auto data = reinterpret_cast<std::uintptr_t>(value.lights.data());
        const auto begin = reinterpret_cast<std::uintptr_t>(&value.lights);
        if (data < begin || data >= begin + sizeof(value.lights))
        {
            bytes += value.lights.capacity() + 1;
        }
        return bytes;
    }

    const FeatureDescriptor ClusteredLightingFeatureDescriptor::descriptor{
        name,
        cxx::typeToken<Config>(),
        "Independent conservative Cluster Build/Count/Scan/Fill with full-light overflow fallback.",
        describeClusters
    };

    const FeatureGraphOps& detail::ClusteredLightingFeature::graph(bool) noexcept
    {
        static const FeatureGraphOps graph{buildClusters};
        return graph;
    }
} // namespace lux::render
