#include <cmath>
#include <lux/engine/render/features/shadow/ShadowMapFeature.hpp>

#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/resource/view/CameraParameters.hpp>
#include <lux/engine/render/features/lighting/LightPorts.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/shadow/ShadowProjectionBinding.hpp>

namespace lux::render
{
    template <> struct ConfigValueTraits<detail::ShadowProjectionBinding::Description> final
    {
        static RenderResult<std::size_t>
        retainedBytes(const detail::ShadowProjectionBinding::Description& value) noexcept
        {
            auto bytes = ConfigValueTraits<ShadowMapFeatureConfig>::retainedBytes(value.config);
            return bytes ? RenderResult<std::size_t>{*bytes + sizeof(value) - sizeof(value.config)}
                         : cxx::unexpected(bytes.error());
        }
    };

    RenderResult<void>
    detail::ShadowMapFeature::Ports::describe(const Config* value, std::string_view, FeaturePorts& ports) noexcept
    {
        const auto& config = *static_cast<const ShadowMapFeatureConfig*>(value);
        const bool invalid_shape = config.pcf_radius > 4 || config.shape.resolution <= 2 * (config.pcf_radius + 2);
        const bool invalid_inputs = config.lights.empty() || !std::isfinite(config.distance) || config.distance <= 0;
        if (invalid_shape || invalid_inputs)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        const auto add = [&](std::string name, std::uint64_t bytes, std::string_view semantic)
        {
            FeaturePort port{
                std::move(name),
                EFeaturePort::OUTPUT,
                GraphBufferDesc{bytes},
                EGraphOrigin::IMPORTED,
                {GraphBufferRange{0, bytes}}
            };
            port.semantic = semantic;
            ports.resources.push_back(std::move(port));
        };
        add("views", std::uint64_t(config.shape.view_count) * sizeof(MeshVisibilityView), kMeshVisibilityViews);
        add("slices", std::uint64_t(config.shape.view_count) * sizeof(ShadowSliceGpu), kShadowSlices);
        add("light_ranges", sizeof(ShadowMappingHeader) + sizeof(LightShadowRange), kLightShadowRanges);
        add("raster_states", std::uint64_t(config.shape.view_count) * sizeof(RasterState), "lux.raster.state.v1");
        describeLightPorts(ports, config.lights, false);
        return {};
    }

    RenderResult<void>
    detail::ShadowMapFeature::build(const Config* value, std::string_view, FeatureBuildContext& context) noexcept
    {
        const auto& config = *static_cast<const ShadowMapFeatureConfig*>(value);
        const std::array<GraphResourceId, 4> outputs{
            context.resource("views").resource,
            context.resource("slices").resource,
            context.resource("light_ranges").resource,
            context.resource("raster_states").resource
        };
        requireLightPorts(context);
        const auto lights = context.resource("light_counts").resource;
        auto description = ConfigValue::make(detail::ShadowProjectionBinding::Description{config, outputs, lights});
        if (!description)
        {
            return cxx::unexpected(description.error());
        }
        InstanceDataRecipe recipe{
            {outputs.begin(), outputs.end()},
            std::move(*description),
            &ShadowProjectionBinding::create,
            {lights}
        };
        recipe.parameter_bytes = sizeof(CameraParameters);
        recipe.parameter_source = EParameterSource::VIEW;
        context.graph.addInstanceData(std::move(recipe));
        return {};
    }

    RenderResult<std::size_t>
    ConfigValueTraits<ShadowMapFeatureConfig>::retainedBytes(const ShadowMapFeatureConfig& value) noexcept
    {
        auto bytes = sizeof(value);
        const auto address = reinterpret_cast<std::uintptr_t>(value.lights.data());
        const auto begin = reinterpret_cast<std::uintptr_t>(&value.lights);
        if (address < begin || address >= begin + sizeof(value.lights))
        {
            bytes += value.lights.capacity() + 1;
        }
        return bytes;
    }

} // namespace lux::render
