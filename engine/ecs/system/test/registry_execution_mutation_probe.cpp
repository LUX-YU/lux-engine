#include <lux/engine/ecs/SystemContext.hpp>
#include <lux/engine/ecs/SystemExecution.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>

namespace
{
    class MutatingSystem final : public lux::ecs::StaticSystemAccess<>
    {
    public:
        MutatingSystem(
            lux::ecs::SystemRegistry& registry,
            lux::ecs::SystemId& id
        ) noexcept
            : registry_(&registry), id_(&id)
        {
        }

        void update(lux::ecs::SystemContext&) noexcept
        {
            (void)registry_->erase(*id_);
        }

    private:
        lux::ecs::SystemRegistry* registry_{};
        lux::ecs::SystemId* id_{};
    };
}

int main()
{
    lux::ecs::World world;
    lux::ecs::SystemRegistry registry;
    lux::ecs::SystemId system;
    const auto result = registry.emplace<MutatingSystem>(registry, system);
    if (!result)
        return 1;
    system = *result;

    lux::ecs::SystemRelations relations;
    auto compilation = lux::ecs::compileSystemTaskGraph(registry, relations);
    if (!compilation)
        return 2;
    lux::ecs::SystemExecutionScratch scratch;
    if (!scratch.prepare(*compilation))
        return 3;
    lux::ecs::EcsExecutionContext context{
        world,
        registry,
        relations,
        scratch,
        0.0F,
        1U
    };
    const auto executed = lux::ecs::executeSystemTaskGraph(
        lux::task::referenceTaskExecutionBackend(),
        *compilation,
        context
    );
    return executed ? 0 : 4;
}
