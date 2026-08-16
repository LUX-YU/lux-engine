#include <lux/engine/render/comm/server/FeatureRegistration.hpp>

// Out-of-line half of the feature-install bridge: here RenderScene is a complete
// type, so the void* the factory boundary hands us can be turned back into a
// RenderScene and driven through its PUBLIC accessors. The template half
// (addFeature<T>) is instantiated in the plugin's own TU.
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render::detail
{
    Expected<FeatureHandle> installFeatureErased(
        void*                          scene,
        std::unique_ptr<RenderFeature> feature)
    {
        return static_cast<RenderScene*>(scene)->addFeatureErased(std::move(feature));
    }

} // namespace lux::render::detail
