#include "ObjectBoundaryProbe.hpp"

#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/ecs/SystemTaskResources.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <array>
#include <cassert>
#include <utility>

namespace
{
    class DemandObserver final : public lux::object::Object<DemandObserver>
    {
    public:
        using Object::Object;

        void receive(const lux::simulation::test::MaterialTextureDemand& demand) noexcept
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

int
main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::ecs;
    using namespace lux::simulation::test;

    lux::object::ObjectMessageQueue queue;
    DemandObserver observer{queue.dispatcherRef()};
    Registry world;
    const Entity entity = world.create();

    SystemRegistry registry;
    const auto id = registry.emplace<MaterialTextureSystem>(queue.dispatcherRef());
    assert(id);
    auto retained = registry.retain<MaterialTextureSystem>(*id);
    assert(retained);
    auto system = std::move(*retained);
    assert(system->prepare(4U));
    const auto weak = system->weakRef();

    auto observed =
        system->observe<MaterialTextureSystem::textureDemand, &DemandObserver::receive, lux::object::EDelivery::DIRECT>(
            observer
        );
    assert(observed);

    EcsCommandBuffer commands;
    constexpr std::array capacities{EcsCommandProducerCapacity{2U, 128U}};
    assert(commands.prepare(capacities));
    lux::task::TaskGraphBuilder builder;
    const auto update = builder.add(systemTaskResources<MaterialTextureSystem>(), [system, &commands]() noexcept {
        auto begun = commands.begin(0U);
        assert(begun);
        auto writer = std::move(*begun);
        system->invokeTask(writer);
    }
    );
    const auto apply = builder.add(
        lux::task::dependsOn(*update),
        lux::task::on(lux::task::ETaskAffinity::CALLER_THREAD),
        ecsCommandFlushTaskResources(),
        [&world, &commands]() noexcept { assert(applyEcsCommands(world, commands)); }
    );
    assert(update && apply);
    auto graph = std::move(builder).build();
    assert(graph);
    lux::task::TaskExecutor executor({0U, graph->taskCount()});

    assert(executor.execute(*graph));
    assert(observer.calls == 1U);
    assert(!world.all_of<MaterialTextureResident>(entity));
    assert(lux::object::postEvent(weak, TextureReady{entity, 7U}) == lux::object::EEventPostStatus::POSTED);
    assert(queue.dispatchPending() == 1U);
    assert(executor.execute(*graph));
    assert(world.get<MaterialTextureResident>(entity).texture == 7U);

    const auto old_revision = registry.revision();
    assert(registry.erase(*id));
    assert(registry.revision() == old_revision + 1U);
    system = {};
    assert(!weak.expired());
    *graph = lux::task::TaskGraph{};
    assert(weak.expired());
}
