#include "ObjectBoundaryProbe.hpp"

#include <lux/engine/ecs/system/detail/SystemTestRig.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

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
    auto mutation_result = world.mutate();
    assert(mutation_result);
    auto mutation = std::move(*mutation_result);
    const Entity entity = mutation.create();
    mutation = {};

    detail::SystemTestRig execution{world};
    const auto id = execution.add<MaterialTextureSystem>(
        queue.dispatcherRef()
    );
    auto& system = execution.system<MaterialTextureSystem>(id);
    const auto weak = system.weakRef();
    auto observed = system.observe<
        MaterialTextureSystem::textureDemand,
        &DemandObserver::receive,
        lux::object::EDelivery::DIRECT>(observer);
    assert(observed);
    assert(execution.compile());

    assert(execution.run(1.0F / 60.0F, 1U));
    assert(observer.calls == 1U);
    assert(observer.observed_material == 3U);
    assert(observer.observed_texture == 7U);
    assert(world.find<MaterialTextureResident>(entity) == nullptr);

    assert(lux::object::postEvent(
        weak,
        TextureReady{entity, 7U}
    ) == lux::object::EEventPostStatus::POSTED);
    assert(world.find<MaterialTextureResident>(entity) == nullptr);
    assert(queue.dispatchPending() == 1U);
    assert(world.find<MaterialTextureResident>(entity) == nullptr);

    assert(execution.run(1.0F / 60.0F, 2U));
    assert(world.get<MaterialTextureResident>(entity).texture == 7U);

    assert(lux::object::postEvent(
        weak,
        TextureReady{entity, 9U}
    ) == lux::object::EEventPostStatus::POSTED);
    assert(execution.erase(id));
    assert(weak.expired());
    assert(queue.dispatchPending() == 1U);
    assert(world.get<MaterialTextureResident>(entity).texture == 7U);
}
