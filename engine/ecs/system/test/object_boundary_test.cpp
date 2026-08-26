#include "ObjectBoundaryProbe.hpp"

#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/WorldTaskResources.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/task/TaskGraph.hpp>

#include <cassert>
#include <memory>

namespace
{
    class DemandObserver final : public lux::object::Object<DemandObserver>
    {
      public:
        using Object::Object;
        void receive(
            const lux::ecs::test::MaterialTextureDemand& demand
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
    using namespace lux::ecs;
    using namespace lux::ecs::test;

    lux::object::ObjectMessageQueue queue;
    DemandObserver observer{queue.dispatcherRef()};
    World world{WorldConfig{{4096U, 16U * 4096U}}};
    auto mutation = world.mutate();
    assert(mutation);
    const Entity entity = mutation->create();
    mutation = {};

    SystemRegistry registry;
    const auto id = registry.emplace<MaterialTextureSystem>(
        queue.dispatcherRef()
    );
    assert(id);
    auto system = registry.retain<MaterialTextureSystem>(*id);
    assert(system);
    const auto weak = system->weakRef();
    auto observed = system->observe<
        MaterialTextureSystem::textureDemand,
        &DemandObserver::receive,
        lux::object::EDelivery::DIRECT>(observer);
    assert(observed);

    WorldChangeBatch changes;
    assert(changes.prepare({}));
    WorldCommandBatch commands;
    assert(commands.prepare(1U));
    lux::task::TaskGraphBuilder builder;
    const auto update = builder.add(
        [&world, system, &changes, &commands]() noexcept
        {
            auto scope = commands.begin(0U);
            assert(scope);
            system->invokeTask(world, changes, scope->commands());
        }
    );
    const auto apply = builder.add(
        lux::task::on(lux::task::ETaskAffinity::CALLER_THREAD),
        worldCommandsWrite(),
        [&world, &commands]() noexcept
        {
            applyWorldCommands(world, commands);
        }
    );
    assert(update && apply && builder.before(*update, *apply));
    auto graph = lux::task::compile(std::move(builder));
    assert(graph);
    lux::task::TaskRunState state;
    assert(lux::task::prepare(state, *graph));
    lux::task::InlineTaskExecutor executor;
    const auto run_frame = [&]()
    {
        auto lease = world.beginTaskExecution();
        assert(lease);
        assert(lux::task::run(*graph, executor, state));
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
    system.reset();
    assert(!weak.expired());
    graph = {};
    assert(weak.expired());
}
