#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>
#include <lux/engine/ecs/World.hpp>

int main()
{
    lux::ecs::World world;
    lux::ecs::SystemRegistry systems;
    lux::ecs::SystemRelations relations;
    return lux::ecs::compileSystemTaskGraph(systems, relations) ? 0 : 1;
}
