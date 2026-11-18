#include <lux/engine/render/capabilities/BuiltinImplementations.hpp>
#include <lux/engine/render/capabilities/RenderPresets.hpp>
#include <lux/engine/render/capabilities/FeatureRecipes.hpp>

namespace lux::render
{
    RenderResult<CreateEffectRequest>
    makeDeferredRenderEffect(
        EffectOutputDesc output,
        const CapabilityImplementationInfo& surface,
        const CapabilityImplementationInfo& lighting,
        const RasterSurfaceConfiguration& surface_config,
        const DeferredLightingConfiguration& lighting_config
    ) noexcept
    {
        const bool is_wrong_surface = surface.implementation != detail::RasterSurface::name.name() ||
            surface.capability.type != cxx::make_basic_type_info<Mesh3dCapability>();
        const bool is_wrong_lighting = lighting.implementation != detail::DeferredLight::name.name() ||
            lighting.capability.type != cxx::make_basic_type_info<Lighting3dCapability>();
        if (is_wrong_surface || is_wrong_lighting) return cxx::unexpected(RenderError{kCapabilityDeclaration});
        CreateEffectRequest request{output};
        auto inputs = detail::appendSurfaceFeatures(
            request.definition, surface.features, output, surface_config, "surface"
        );
        if (!inputs) return cxx::unexpected(inputs.error());
        auto ports = detail::appendLightingFeatures(
            request.definition, lighting.features, output, lighting_config, "lighting"
        );
        if (!ports) return cxx::unexpected(ports.error());
        for (std::size_t i = 0; i < inputs->size(); ++i)
            request.definition.connections.push_back({(*inputs)[i], ports->surface[i]});
        request.definition.outputs.push_back({ports->color, GraphOutputRequirements{output.format, output.samples}});
        request.implementations = {{surface.implementation_id, "surface"}, {lighting.implementation_id, "lighting"}};
        return request;
    }

    CapabilityCompositionDescription deferredRenderComposition(bool clustered, bool shadows) noexcept
    {
        DeferredLightingConfiguration lighting;
        lighting.clustered = clustered;
        lighting.shadows = shadows;
        CapabilityCompositionDescription result;
        result.contributions.push_back(
            {"surface",
             std::string(kRasterSurfaceCapability),
             std::string(detail::RasterSurface::name.name()),
             *encodeRasterSurfaceConfiguration({})}
        );
        result.contributions.push_back(
            {"lighting",
             std::string(kDeferredLightingCapability),
             std::string(detail::DeferredLight::name.name()),
             *encodeDeferredLightingConfiguration(lighting)}
        );
        result.outputs.push_back({"lighting", "color"});
        return result;
    }
} // namespace lux::render
