#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <utility>

int
main()
{
    lux::world::WorldDescriptionBuilder builder;
    auto world = std::move(builder).build();
    return world && world->empty() ? 0 : 1;
}
