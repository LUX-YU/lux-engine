#include <lux/engine/ecs/SystemExecution.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/object/Object.hpp>

#include <cstdint>

namespace
{
    class ObjectSystem final
        : public lux::object::Object<ObjectSystem>,
          public lux::ecs::StaticSystemAccess<>
    {
      public:
        explicit ObjectSystem(std::uint32_t& count) noexcept : count_(&count) {}

        void update(lux::ecs::SystemContext&) noexcept
        {
            ++*count_;
        }

      private:
        std::uint32_t* count_{};
    };
}

int main()
{
    lux::ecs::World world;
    lux::ecs::SystemRegistry systems;
    std::uint32_t count{};
    if (!systems.emplace<ObjectSystem>(count))
        return 1;
    lux::ecs::SystemRelations relations;
    auto compilation = lux::ecs::compileSystemTaskGraph(systems, relations);
    if (!compilation)
        return 2;
    lux::ecs::SystemExecutionScratch scratch;
    if (!scratch.prepare(*compilation))
        return 3;
    lux::ecs::EcsExecutionContext context{
        world, systems, relations, scratch, 1.0F / 60.0F, 1U
    };
    if (!lux::ecs::executeSystemTaskGraph(
            lux::task::referenceTaskExecutionBackend(),
            *compilation,
            context
        ))
    {
        return 4;
    }
    return count == 1U ? 0 : 5;
}
