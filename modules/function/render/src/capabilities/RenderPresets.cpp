#include <lux/engine/render/capabilities/BuiltinImplementations.hpp>
#include <lux/engine/render/capabilities/RenderPresets.hpp>

namespace lux::render
{
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
