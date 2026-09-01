#include <lux/engine/scene/RenderSyncPipeline.hpp>

#include <cstdint>

int main()
{
    const auto render_entity = lux::scene::toRenderEntity(static_cast<lux::simulation::ecs::Entity>(0x42ULL));
    return static_cast<std::uint64_t>(render_entity) == 0x42ULL ? 0 : 1;
}
