#include "DeferredLighting.hpp"
#include "DeferredLightingClustered.hpp"
#include "DeferredLightingClusteredShadow.hpp"
#include "DeferredLightingShadow.hpp"
#include <cstring>
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/resource/view/CameraParameters.hpp>
#include <lux/engine/render/features/lighting/LightPorts.hpp>
#include <lux/engine/render/features/mesh/GBufferFeature.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/lighting/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/common/FullscreenRaster.hpp>

namespace lux::render
{
    RenderResult<void>
    detail::DeferredLightingFeature::Ports::describe(
        const Config* value,
        std::string_view,
        FeaturePorts& result
    ) noexcept
    {
        const auto& config = *static_cast<const DeferredLightingFeatureConfig*>(value);
        const bool invalid_extent = (config.extent.extent.source == EImageExtentSource::FIXED &&
            (!config.extent.width || !config.extent.height)) || config.extent.mips != 1 ||
                                    config.extent.layers != 1 || config.extent.samples != 1;
        const bool missing_lights = !config.external_lights && config.lights.empty();
        const bool invalid_clusters =
            config.clusters && (config.clusters->cluster_count == 0 || config.clusters->index_capacity == 0);
        const bool invalid_configuration = invalid_extent || missing_lights || invalid_clusters;
        if (invalid_configuration)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        for (std::size_t i = 0; i < kGBufferPorts.size(); ++i)
        {
            auto image = config.extent;
            image.format = i == 3 ? rdesc::GBufferLayout::depth_format : rdesc::GBufferLayout::color_formats[i];
            FeaturePort port{std::string(kGBufferPorts[i]), EFeaturePort::INPUT, image};
            port.semantic = std::string(rdesc::GBufferLayout::semantic) + "." + std::string(kGBufferPorts[i]);
            result.resources.push_back(std::move(port));
        }
        describeLightPorts(result, config.lights, config.external_lights);
        if (config.shadows)
        {
            const auto& shape = *config.shadows;
            const auto buffer = [&](std::string name, std::uint64_t size, std::string_view semantic)
            {
                FeaturePort port{std::move(name), EFeaturePort::INPUT, GraphBufferDesc{size}};
                port.semantic = semantic;
                result.resources.push_back(std::move(port));
            };
            buffer(
                "shadow_views",
                std::uint64_t(shape.view_count) * sizeof(MeshVisibilityView),
                kMeshVisibilityViews
            );
            buffer("shadow_slices", std::uint64_t(shape.view_count) * sizeof(ShadowSliceGpu), kShadowSlices);
            buffer("shadow_ranges", sizeof(ShadowMappingHeader) + sizeof(LightShadowRange), kLightShadowRanges);
            FeaturePort depth{"shadow_depth", EFeaturePort::INPUT, shape.image()};
            depth.semantic = kShadowDepth;
            result.resources.push_back(std::move(depth));
        }
        if (config.clusters)
        {
            const auto port = [&](std::string name, std::uint64_t bytes, std::string_view semantic)
            {
                FeaturePort input{std::move(name), EFeaturePort::INPUT, GraphBufferDesc{bytes}};
                input.semantic = semantic;
                result.resources.push_back(std::move(input));
            };
            port("cluster_grid", sizeof(ClusterGridGpu), kClusterGrid);
            port("cluster_offsets", (std::uint64_t(config.clusters->cluster_count) + 1) * 4, kClusterOffsets);
            port("cluster_indices", std::uint64_t(config.clusters->index_capacity) * 4, kClusterIndices);
            port("cluster_status", sizeof(ClusterStatusGpu), kClusterStatus);
        }
        auto image = config.extent;
        image.format = rdesc::ETextureFormat::RGBA16_SFLOAT;
        FeaturePort hdr{"hdr_color", EFeaturePort::OUTPUT, image};
        hdr.semantic = "lux.color.linear-hdr.v1";
        result.resources.push_back(std::move(hdr));
        return {};
    }

    RenderResult<void>
    detail::DeferredLightingFeature::build(
        const Config* value,
        std::string_view key,
        FeatureBuildContext& context
    ) noexcept
    {
        const auto& config = *static_cast<const DeferredLightingFeatureConfig*>(value);
        constexpr std::array shaders{
            &kDeferredLighting, &kDeferredLightingShadow, &kDeferredLightingClustered, &kDeferredLightingClusteredShadow
        };
        const auto variant = 2 * unsigned(config.clusters.has_value()) + unsigned(config.shadows.has_value());
        auto work = fullscreenRaster(context, GraphProgram{*shaders[variant]});
        if (!work)
        {
            return cxx::unexpected(work.error());
        }
        for (auto name : kGBufferPorts)
        {
            work->fixed().interface.resources.push_back({std::string(name), {context.resource(name)}});
        }
        auto& shader = work->fixed().interface;
        for (const auto name : kLightPorts)
        {
            shader.resources.push_back({name, {context.resource(name)}});
        }
        if (!config.external_lights) requireLightPorts(context);
        if (config.shadows)
        {
            for (auto name : {"shadow_views", "shadow_slices", "shadow_ranges", "shadow_depth"})
            {
                work->fixed().interface.resources.push_back({name, {context.resource(name)}});
            }
            const auto sampler = context.graph.addSampler(
                {ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_BORDER, EDepthCompare::LESS_EQUAL, 1}
            );
            work->fixed().interface.samplers.push_back({"shadow_compare", {sampler}, kGraphInvalidIndex, true});
        }
        if (config.clusters)
        {
            for (auto name : {"cluster_grid", "cluster_offsets", "cluster_indices", "cluster_status"})
            {
                work->fixed().interface.resources.push_back({name, {context.resource(name)}});
            }
        }
        auto& parameters = shader.parameters;
        parameters.source = EParameterSource::VIEW;
        parameters.instance_layout = cameraParameterLayout();
        parameters.fields = {
            {"inverse_view_projection", {}, "inverse_view_projection"},
            {"camera_position", {}, "position_near"},
            {"viewport", {}, "viewport"},
            {"background", {}, "background"}
        };
        constexpr std::size_t environment_offset = 112; // Shared push-constant layout of the four variants.
        if (parameters.values.size() < environment_offset + sizeof(config.environment))
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        std::memcpy(parameters.values.data() + environment_offset, config.environment.data(), sizeof(config.environment));
        work->colors.push_back({{context.resource("hdr_color")}});
        auto pass = context.graph.addRasterPass(
            featureKey(
                detail::DeferredLightingFeature::name,
                key,
                config.clusters ? "clustered lights" : "all lights"
            ),
            std::move(*work)
        );
        if (!pass)
        {
            return cxx::unexpected(pass.error());
        }
        return context.publish("hdr_color", pass->written.front());
    }

    RenderResult<std::size_t>
    ConfigValueTraits<DeferredLightingFeatureConfig>::retainedBytes(const DeferredLightingFeatureConfig& value) noexcept
    {
        auto bytes = sizeof(value);
        const auto begin = reinterpret_cast<std::uintptr_t>(&value.lights);
        const auto data = reinterpret_cast<std::uintptr_t>(value.lights.data());
        if (data < begin || data >= begin + sizeof(value.lights))
        {
            bytes += value.lights.capacity() + 1;
        }
        return bytes;
    }

} // namespace lux::render
