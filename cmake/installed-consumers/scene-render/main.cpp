#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/RenderRuntime.hpp>
#include <lux/engine/scene/RenderSyncPipeline.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>

#include <cstdint>

int main()
{
    const auto render_entity = lux::scene::toRenderEntity(static_cast<lux::simulation::ecs::Entity>(0x42ULL));
    const auto registration = lux::scene::builtinRenderSystemRegistration();
    const bool valid = static_cast<std::uint64_t>(render_entity) == 0x42ULL &&
        registration.configuration.valid() && lux::scene::builtinRenderFeatureSceneBindings().size() == 2U &&
        lux::scene::sceneRenderComponentSchemas().size() == 1U;
    return valid ? 0 : 1;
}
