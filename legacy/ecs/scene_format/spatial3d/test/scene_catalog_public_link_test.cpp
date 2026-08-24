#include <lux/engine/ecs/scene_format/spatial3d/SceneCatalog.hpp>

int main()
{
    const lux::ecs::scene_format::spatial3d::SceneCatalog empty;
    const auto validated =
        lux::ecs::scene_format::spatial3d::validateSceneCatalog(empty);
    return validated ? 1 : 0;
}
