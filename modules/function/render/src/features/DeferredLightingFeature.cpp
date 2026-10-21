#include "DeferredLighting.hpp"
#include "DeferredLightingShadow.hpp"
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/FeatureDefinition.hpp>
#include <lux/engine/render/features/FullscreenRaster.hpp>
#include <lux/engine/render/runtime/Registration.hpp>
#include <lux/engine/render/storage/DirectLightingCommandHandler.hpp>

namespace lux::render
{
    namespace
    {
        constexpr std::array<std::string_view, 4>
            surfaces{"albedo_metallic", "normal_roughness", "emissive_ao", "depth"};
        constexpr std::array<std::string_view, 4>
            lights{"directional_lights", "point_lights", "spot_lights", "light_counts"};
        constexpr std::array<std::uint32_t, 4> strides{32, 48, 64, 16};

        RenderResult<void> describeLighting(const void* value, std::string_view, FeaturePorts& result) noexcept
        {
            const auto& config = *static_cast<const DeferredLightingFeatureConfig*>(value);
            const bool invalid_extent = !config.extent.width || !config.extent.height || config.extent.mips != 1 ||
                                        config.extent.layers != 1 || config.extent.samples != 1;
            if (invalid_extent || config.lights.empty())
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            for (std::size_t i = 0; i < surfaces.size(); ++i)
            {
                auto image = config.extent;
                image.format = i == 3 ? rdesc::GBufferLayout::depth_format : rdesc::GBufferLayout::color_formats[i];
                FeaturePort port{std::string(surfaces[i]), EFeaturePort::INPUT, image};
                port.semantic = std::string(rdesc::GBufferLayout::semantic) + "." + std::string(surfaces[i]);
                result.resources.push_back(std::move(port));
                if (config.shadows)
                {
                    result.resources.push_back(
                        {std::string(lights[i]), EFeaturePort::INPUT, GraphBufferDesc{strides[i]}}
                    );
                }
                else
                {
                    result.storage.scene.push_back(
                        {std::string(lights[i]),
                         EFeaturePort::OUTPUT,
                         GraphBufferDesc{strides[i]},
                         EGraphOrigin::IMPORTED,
                         {GraphBufferRange{0, strides[i]}},
                         StorageRequirement{StorageName(kDirectLightingStorageType.name()), config.lights}}
                    );
                }
            }
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
            auto image = config.extent;
            image.format = rdesc::ETextureFormat::RGBA16_SFLOAT;
            FeaturePort hdr{"hdr_color", EFeaturePort::OUTPUT, image};
            hdr.semantic = "lux.color.linear-hdr.v1";
            result.resources.push_back(std::move(hdr));
            return {};
        }

        RenderResult<void> buildLighting(const void* value, std::string_view key, FeatureBuildContext& context) noexcept
        {
            const auto& config = *static_cast<const DeferredLightingFeatureConfig*>(value);
            auto work =
                config.shadows
                    ? detail::fullscreenRaster(context, kDeferredLightingShadow, kDeferredLightingShadowReflection)
                    : detail::fullscreenRaster(context, kDeferredLighting, kDeferredLightingReflection);
            if (!work)
            {
                return cxx::unexpected(work.error());
            }
            for (auto name : surfaces)
            {
                work->interface.resources.push_back({std::string(name), {context.resource(name)}});
            }
            for (std::uint32_t i = 0; i < lights.size(); ++i)
            {
                auto resource = context.resource(lights[i]);
                work->interface.resources.push_back({std::string(lights[i]), {resource}});
                if (!config.shadows)
                {
                    context.arguments.push_back(StorageArgument::make(
                        resource.resource,
                        DirectLightingBinding{static_cast<EDirectLightingBuffer>(i)}
                    ));
                }
            }
            if (config.shadows)
            {
                for (auto name : {"shadow_views", "shadow_slices", "shadow_ranges", "shadow_depth"})
                {
                    work->interface.resources.push_back({name, {context.resource(name)}});
                }
                const auto sampler = context.addSampler(
                    {ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_BORDER, EDepthCompare::LESS_EQUAL, 1}
                );
                work->interface.samplers.push_back({"shadow_compare", {sampler}, kGraphInvalidIndex, true});
            }
            work->interface.parameters.instance_layout = config.input_layout;
            for (auto name : {"inverse_view_projection", "camera_position", "viewport", "background", "environment"})
            {
                work->interface.parameters.fields.push_back({name, {}, name});
            }
            work->colors.push_back({{context.resource("hdr_color")}});
            auto pass = context.graph.addRasterPass(
                featureKey(DeferredLightingFeatureDescriptor::name, key, "all lights"),
                std::move(*work)
            );
            if (!pass)
            {
                return cxx::unexpected(pass.error());
            }
            return context.publish("hdr_color", pass->written.front());
        }
    } // namespace

    RenderResult<std::size_t> ConfigValueTraits<DeferredLightingFeatureConfig>::retainedBytes(
        const DeferredLightingFeatureConfig& value
    ) noexcept
    {
        auto bytes = sizeof(value) + detail::parameterLayoutBytes(value.input_layout);
        const auto begin = reinterpret_cast<std::uintptr_t>(&value.lights);
        const auto data = reinterpret_cast<std::uintptr_t>(value.lights.data());
        if (data < begin || data >= begin + sizeof(value.lights))
        {
            bytes += value.lights.capacity() + 1;
        }
        return bytes;
    }

    const FeatureDescriptor DeferredLightingFeatureDescriptor::descriptor{
        name,
        cppType<Config>(),
        "All-lights deferred shading; optional shadow inputs are supplied through independent ports.",
        describeLighting
    };

    template <>
    RenderResult<void> RenderServer::prepareFeature<DeferredLightingFeatureDescriptor>(
        RegistrationBatch& batch,
        std::string_view
    ) noexcept
    {
        if (auto result = prepareStorage<DirectLightingStorageDescriptor>(batch, {}, true, {}); !result)
        {
            return result;
        }
        static const FeatureGraphOps graph{buildLighting};
        batch.addFeature({&DeferredLightingFeatureDescriptor::descriptor, &graph});
        return {};
    }

    template LUX_RENDER_PUBLIC RenderResult<FeatureId> RenderServer::registerFeature<DeferredLightingFeatureDescriptor>(
        std::string_view
    ) noexcept;
} // namespace lux::render
