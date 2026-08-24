#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/schedule/detail/ScheduleTestAccess.hpp>

#include <cassert>
#include <memory>
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
        Producer(lux::ecs::Entity entity, std::vector<int>& order) noexcept
            : entity_(entity), order_(&order)
        {
        }

        [[nodiscard]] lux::ecs::SystemAccess access() const noexcept override
        {
            return lux::ecs::access(lux::ecs::query<lux::ecs::Read<Counter>>());
        }

        void update(lux::ecs::SystemFrame& frame) noexcept override
        {
            order_->push_back(1);
            if (frame.find<Counter>(entity_) == nullptr)
            {
                assert(frame.commands().push(AddCounter{entity_}) ==
                    lux::ecs::ECommandResult::ACCEPTED);
            }
        }

      private:
        lux::ecs::Entity entity_;
        std::vector<int>* order_{};
    };

    class Consumer final : public lux::ecs::System
    {
      public:
        Consumer(
            lux::ecs::Entity entity,
            std::vector<int>& order,
            int marker
        ) noexcept
            : entity_(entity), order_(&order), marker_(marker)
        {
        }

        [[nodiscard]] lux::ecs::SystemAccess access() const noexcept override
        {
            return lux::ecs::access(lux::ecs::query<lux::ecs::Read<Counter>>());
        }

        void update(lux::ecs::SystemFrame& frame) noexcept override
        {
            assert(frame.get<Counter>(entity_).value == 1);
            order_->push_back(marker_);
        }

      private:
        lux::ecs::Entity entity_;
        std::vector<int>* order_{};
        int marker_{};
    };
}

int main()
{
    using namespace lux::ecs;

    World world;
    auto world_edit_result = world.edit();
    assert(world_edit_result);
    auto world_edit = std::move(*world_edit_result);
    const Entity entity = world_edit.create();
    world_edit = {};

    std::vector<int> order;
    Schedule schedule(world);
    auto edit_result = schedule.edit();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    const auto producer = edit.add(
        std::make_unique<Producer>(entity, order),
        SystemPhase::PreUpdate
    );
    const auto first = edit.add(
        std::make_unique<Consumer>(entity, order, 2),
        SystemPhase::Update
    );
    const auto second = edit.add(
        std::make_unique<Consumer>(entity, order, 3),
        SystemPhase::Update
    );
    assert(producer && first && second);
    edit.before(first, second);
    edit.after(first, systemSetId("optional.absent"));
    assert(edit.commit());

    schedule.run(1.0F / 60.0F, 1);
    assert((order == std::vector<int>{1, 2, 3}));
    assert(world.get<Counter>(entity).value == 1);

    const auto plan = detail::ScheduleTestAccess::snapshot(schedule);
    assert(plan.order.size() == 3);
    assert(plan.batches.size() == 3);
}
