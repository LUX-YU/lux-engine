#include <lux/engine/spatial3d/SceneCatalog.hpp>

int main()
{
    const lux::spatial3d::SceneCatalog empty;
    const auto validated = lux::spatial3d::validateSceneCatalog(empty);
    return validated ? 1 : 0;
}
