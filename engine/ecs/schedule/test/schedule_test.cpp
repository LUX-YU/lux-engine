#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/schedule/detail/ScheduleTestAccess.hpp>
#include <lux/engine/object/Object.hpp>

#include <cassert>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace
{
    struct Counter final
    {
        int value{};
    };

    struct AddCounter final
    {
        lux::ecs::Entity entity{lux::ecs::NullEntity};

        void apply(lux::ecs::WorldEdit& edit) noexcept
        {
            edit.emplace<Counter>(entity, 1);
        }
    };

    class Producer final : public lux::ecs::System
    {
      public:
        explicit Producer(lux::ecs::Entity entity, std::vector<int>& order)
            : entity_(entity), order_(&order)
        {
        }

        void update(const lux::ecs::SystemFrame& frame) noexcept override
        {
            order_->push_back(1);
            if (frame.find<Counter>(entity_) == nullptr)
            {
                const auto result = frame.commands().push(AddCounter{entity_});
                assert(result == lux::ecs::ECommandResult::ACCEPTED);
            }
        }

        std::span<const lux::ecs::SystemSetId> sets() const noexcept override
        {
            static constexpr lux::ecs::SystemSetId sets[]{
                lux::ecs::systemSetId("test.producer")};
            return sets;
        }

      private:
        lux::ecs::Entity entity_;
        std::vector<int>* order_{};
    };

    class Consumer final : public lux::ecs::System
    {
      public:
        explicit Consumer(std::vector<int>& order) : order_(&order) {}

        void update(const lux::ecs::SystemFrame&) noexcept override
        {
            order_->push_back(2);
        }

        std::span<const lux::ecs::SystemOrder> ordering() const noexcept override
        {
            static constexpr lux::ecs::SystemOrder order[]{
                {lux::ecs::ESystemOrder::AFTER,
                 lux::ecs::systemSetId("test.producer"), true}};
            return order;
        }

      private:
        std::vector<int>* order_{};
    };

    class ObjectProducer final
        : public lux::object::Object<ObjectProducer>,
          public lux::ecs::System
    {
      public:
        explicit ObjectProducer(int& updates) noexcept : updates_(&updates) {}

        void update(const lux::ecs::SystemFrame&) noexcept override
        {
            ++*updates_;
        }

      private:
        int* updates_{};
    };
}

int main()
{
    static_assert(!std::derived_from<lux::ecs::System, lux::object::LuxObject>);
    static_assert(std::derived_from<ObjectProducer, lux::ecs::System>);
    static_assert(std::derived_from<ObjectProducer, lux::object::LuxObject>);

    lux::ecs::World world;
    auto world_edit_result = world.edit();
    assert(world_edit_result);
    auto world_edit = std::move(*world_edit_result);
    const lux::ecs::Entity entity = world_edit.create();
    world_edit = {};

    std::vector<int> order;
    lux::ecs::Schedule schedule(world);
    auto edit_result = schedule.edit();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    int object_updates{};
    const auto consumer = edit.add(std::make_unique<Consumer>(order));
    const auto producer = edit.add(std::make_unique<Producer>(entity, order));
    const auto object_producer = edit.add(
        std::make_unique<ObjectProducer>(object_updates)
    );
    assert(consumer && producer && object_producer);
    assert(edit.commit());

    schedule.run(1.0F / 60.0F, 1);
    assert((order == std::vector<int>{1, 2}));
    assert(object_updates == 1);
    assert(world.get<Counter>(entity).value == 1);
    assert(schedule.get(producer) != nullptr);

    const auto plan = lux::ecs::detail::ScheduleTestAccess::snapshot(schedule);
    bool object_owner_thread_only{};
    for (const auto& entry : plan.order)
    {
        if (entry.type == lux::cxx::typeToken<ObjectProducer>())
            object_owner_thread_only = entry.owner_thread_only;
    }
    assert(object_owner_thread_only);
    bool object_singleton_batch{};
    for (const auto& batch : plan.batches)
    {
        if (batch.size() != 1u)
            continue;
        const auto slot = batch.front();
        for (const auto& entry : plan.order)
        {
            if (entry.slot == slot &&
                entry.type == lux::cxx::typeToken<ObjectProducer>())
            {
                object_singleton_batch = true;
            }
        }
    }
    assert(object_singleton_batch);

    lux::ecs::World other_world;
    lux::ecs::Schedule other(other_world);
    assert(other.get(producer) == nullptr);
}
