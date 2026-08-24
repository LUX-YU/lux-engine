#include "ObjectBoundaryProbe.hpp"

#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/schedule/detail/ScheduleTestAccess.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

#include <cassert>
#include <memory>
#include <utility>

namespace
{
    class DemandObserver final : public lux::object::Object<DemandObserver>
    {
      public:
        using Object::Object;

        void receive(
            const lux::ecs::test::SectionDemandChanged& demand
        ) noexcept
        {
            observed_section = demand.section;
            ++calls;
        }

        std::uint32_t observed_section{};
        std::uint32_t calls{};
    };
}

int main()
{
    using namespace lux::ecs;
    using namespace lux::ecs::test;

    lux::object::ObjectMessageQueue queue;
    DemandObserver observer{queue.dispatcherRef()};
    World world;
    auto world_edit_result = world.edit();
    assert(world_edit_result);
    auto world_edit = std::move(*world_edit_result);
    const Entity entity = world_edit.create();
    world_edit = {};

    Schedule schedule{world};
    auto system = std::make_unique<StreamingDemandSystem>(queue.dispatcherRef());
    const auto weak = system->weakRef();
    auto observed = system->observe<
        StreamingDemandSystem::demandChanged,
        &DemandObserver::receive,
        lux::object::EDelivery::DIRECT>(observer);
    assert(observed);

    auto edit_result = schedule.edit();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    const auto handle = edit.add(std::move(system));
    assert(handle);
    assert(edit.commit());

    const auto plan = detail::ScheduleTestAccess::snapshot(schedule);
    bool owner_thread_only{};
    bool singleton_batch{};
    std::uint32_t object_slot{};
    for (const auto& entry : plan.order)
    {
        if (entry.type == lux::cxx::typeToken<StreamingDemandSystem>())
        {
            owner_thread_only = entry.owner_thread_only;
            object_slot = entry.slot;
        }
    }
    for (const auto& batch : plan.batches)
    {
        if (batch.size() == 1u && batch.front() == object_slot)
            singleton_batch = true;
    }
    assert(owner_thread_only && singleton_batch);

    schedule.run(1.0F / 60.0F, 1u);
    assert(observer.calls == 1u && observer.observed_section == 7u);
    assert(world.find<SectionResident>(entity) == nullptr);

    assert(lux::object::postEvent(
        weak,
        SectionReady{entity, 7u}
    ) == lux::object::EEventPostStatus::POSTED);
    assert(world.find<SectionResident>(entity) == nullptr);
    assert(queue.dispatchPending() == 1u);
    assert(world.find<SectionResident>(entity) == nullptr);

    schedule.run(1.0F / 60.0F, 2u);
    assert(world.get<SectionResident>(entity).section == 7u);

    assert(lux::object::postEvent(
        weak,
        SectionReady{entity, 9u}
    ) == lux::object::EEventPostStatus::POSTED);
    auto remove_result = schedule.edit();
    assert(remove_result);
    auto remove = std::move(*remove_result);
    remove.remove(handle);
    assert(remove.commit());
    assert(weak.expired());
    assert(queue.dispatchPending() == 1u);
    assert(world.get<SectionResident>(entity).section == 7u);
}
