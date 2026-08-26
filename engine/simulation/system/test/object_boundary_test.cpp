#include "ObjectBoundaryProbe.hpp"

#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/ecs/SystemTaskResources.hpp>
#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <cassert>
#include <memory>

namespace
{
    class DemandObserver final : public lux::object::Object<DemandObserver>
    {
      public:
        using Object::Object;
        void receive(
            const lux::simulation::ecs::test::MaterialTextureDemand& demand
        ) noexcept
        {
            observed_material = demand.material;
            observed_texture = demand.texture;
            ++calls;
        }
        std::uint32_t observed_material{};
        std::uint32_t observed_texture{};
        std::uint32_t calls{};
    };
}

int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::test;

    lux::object::ObjectMessageQueue queue;
    DemandObserver observer{queue.dispatcherRef()};
    EcsState world;
    EcsChangeJournal journal(EcsChangeHistoryBudget{4096U, 16U * 4096U});
    auto mutation = world.mutate();
    assert(mutation);
    const Entity entity = mutation->create();
    mutation = {};

    SystemRegistry registry;
    const auto id = registry.emplace<MaterialTextureSystem>(
        queue.dispatcherRef()
    );
    assert(id);
    auto retained = registry.retain<MaterialTextureSystem>(*id);
    assert(retained);
    auto system = std::move(*retained);
    const auto weak = system->weakRef();
    auto observed = system->observe<
        MaterialTextureSystem::textureDemand,
        &DemandObserver::receive,
        lux::object::EDelivery::DIRECT>(observer);
    assert(observed);

    EcsChangeBatch changes;
    assert(changes.prepare({}));
    EcsCommandBatch commands;
    assert(commands.prepare(1U));
    lux::task::TaskGraphBuilder builder;
    const auto update = builder.add(
        systemTaskResources<MaterialTextureSystem>(),
        [&world, system, &changes, &commands]() noexcept
        {
            auto scope = commands.begin(0U);
            assert(scope);
            system->invokeTask(world, changes, scope->commands());
        }
    );
    const auto apply = builder.add(
        lux::task::dependsOn(*update),
        lux::task::on(lux::task::ETaskAffinity::CALLER_THREAD),
        ecsCommandsWrite(),
        [&world, &journal, &commands]() noexcept
        {
            applyEcsCommands(world, journal, commands);
        }
    );
    assert(update && apply);
    auto graph = std::move(builder).build();
    assert(graph);
    lux::task::TaskExecutor executor({0U, graph->taskCount()});
    const auto run_frame = [&]()
    {
        assert(executor.execute(*graph));
    };

    run_frame();
    assert(observer.calls == 1U);
    assert(world.find<MaterialTextureResident>(entity) == nullptr);
    assert(lux::object::postEvent(weak, TextureReady{entity, 7U}) ==
           lux::object::EEventPostStatus::POSTED);
    assert(queue.dispatchPending() == 1U);
    run_frame();
    assert(world.get<MaterialTextureResident>(entity).texture == 7U);

    const auto old_revision = registry.revision();
    assert(registry.erase(*id));
    assert(registry.revision() == old_revision + 1U);
    system = {};
    assert(!weak.expired());
    *graph = lux::task::TaskGraph{};
    assert(weak.expired());
}
