#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/schedule/detail/ScheduleTestAccess.hpp>

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace
{
    std::atomic_size_t allocations{};
    std::atomic_bool count_allocations{};
}

void* operator new(std::size_t size)
{
    if (count_allocations.load(std::memory_order_relaxed))
        allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* value = std::malloc(size == 0 ? 1 : size))
        return value;
    throw std::bad_alloc();
}

void operator delete(void* value) noexcept
{
    std::free(value);
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete[](void* value) noexcept
{
    ::operator delete(value);
}

namespace
{
    struct Position final
    {
        int value{};
    };

    struct Folded final
    {
        bool value{true};
    };

    struct Noop final
    {
        void apply(lux::ecs::WorldEdit&) noexcept {}
    };

    class Empty final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame&) noexcept override {}
    };

    class MissingType final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame&) noexcept override {}
    };

    class RemovableProvider final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame&) noexcept override {}
        [[nodiscard]] bool removable() const noexcept override { return true; }
    };

    class HardConsumer final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame&) noexcept override {}

        std::span<const lux::cxx::TypeToken> requiredSystems() const noexcept override
        {
            static const lux::cxx::TypeToken required[]{
                lux::cxx::typeToken<RemovableProvider>()};
            return required;
        }
    };

    class RequiresMissingType final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame&) noexcept override {}

        std::span<const lux::cxx::TypeToken> requiredSystems() const noexcept override
        {
            static const lux::cxx::TypeToken required[]{
                lux::cxx::typeToken<MissingType>()};
            return required;
        }
    };

    class MissingSet final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame&) noexcept override {}

        std::span<const lux::ecs::SystemOrder> ordering() const noexcept override
        {
            static constexpr lux::ecs::SystemOrder order[]{
                {lux::ecs::ESystemOrder::AFTER,
                 lux::ecs::systemSetId("test.missing"), true}};
            return order;
        }
    };

    class CycleA final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame&) noexcept override {}

        std::span<const lux::ecs::SystemSetId> sets() const noexcept override
        {
            static constexpr lux::ecs::SystemSetId result[]{
                lux::ecs::systemSetId("test.cycle.a")};
            return result;
        }

        std::span<const lux::ecs::SystemOrder> ordering() const noexcept override
        {
            static constexpr lux::ecs::SystemOrder result[]{
                {lux::ecs::ESystemOrder::AFTER,
                 lux::ecs::systemSetId("test.cycle.b"), true}};
            return result;
        }
    };

    class CycleB final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame&) noexcept override {}

        std::span<const lux::ecs::SystemSetId> sets() const noexcept override
        {
            static constexpr lux::ecs::SystemSetId result[]{
                lux::ecs::systemSetId("test.cycle.b")};
            return result;
        }

        std::span<const lux::ecs::SystemOrder> ordering() const noexcept override
        {
            static constexpr lux::ecs::SystemOrder result[]{
                {lux::ecs::ESystemOrder::AFTER,
                 lux::ecs::systemSetId("test.cycle.a"), true}};
            return result;
        }
    };

    class UpdateSet final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame&) noexcept override {}

        std::span<const lux::ecs::SystemSetId> sets() const noexcept override
        {
            static constexpr lux::ecs::SystemSetId result[]{
                lux::ecs::systemSetId("test.update")};
            return result;
        }
    };

    class ContradictoryPre final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame&) noexcept override {}

        std::span<const lux::ecs::SystemOrder> ordering() const noexcept override
        {
            static constexpr lux::ecs::SystemOrder result[]{
                {lux::ecs::ESystemOrder::AFTER,
                 lux::ecs::systemSetId("test.update"), true}};
            return result;
        }
    };

    struct AddFolded final
    {
        lux::ecs::Entity entity{lux::ecs::NullEntity};

        void apply(lux::ecs::WorldEdit& edit) noexcept
        {
            edit.emplace<Folded>(entity);
        }
    };

    class FoldingObserver final : public lux::ecs::System
    {
      public:
        explicit FoldingObserver(bool& detached) noexcept : detached_(&detached) {}

        void onAttach(lux::ecs::SystemAttach& attach) noexcept override
        {
            commands_ = attach.commands();
            attach.observeConstruct<Position, &FoldingObserver::onPosition>(*this);
        }

        void onDetach(lux::ecs::SystemDetach&) noexcept override
        {
            *detached_ = true;
        }

        void update(const lux::ecs::SystemFrame&) noexcept override {}

        [[nodiscard]] bool removable() const noexcept override
        {
            return true;
        }

        void onPosition(lux::ecs::Entity entity) noexcept
        {
            const auto result = commands_.push(AddFolded{entity});
            if (result != lux::ecs::ECommandResult::ACCEPTED)
                ++errors_;
        }

        [[nodiscard]] std::size_t errors() const noexcept
        {
            return errors_;
        }

      private:
        bool* detached_{};
        lux::ecs::WorldCommands commands_;
        std::size_t errors_{};
    };

    struct FinalCommand final
    {
        int* applied{};

        void apply(lux::ecs::WorldEdit&) noexcept
        {
            ++*applied;
        }
    };

    struct ChainCommand final
    {
        lux::ecs::WorldCommands commands;
        int* first{};
        int* final{};
        int* errors{};

        void apply(lux::ecs::WorldEdit&) noexcept
        {
            ++*first;
            if (commands.push(FinalCommand{final}) !=
                lux::ecs::ECommandResult::ACCEPTED)
            {
                ++*errors;
            }
        }
    };

    struct Lifetime final
    {
        explicit Lifetime(int& destroyed) noexcept : destroyed_(&destroyed) {}
        ~Lifetime() noexcept { ++*destroyed_; }
        int* destroyed_{};
    };

    struct OwningCommand final
    {
        std::unique_ptr<Lifetime> value;
        void apply(lux::ecs::WorldEdit&) noexcept {}
    };

    class CommandSystem final : public lux::ecs::System
    {
      public:
        CommandSystem(
            int& first,
            int& final,
            int& errors,
            int& destroyed
        ) noexcept
            : first_(&first), final_(&final), errors_(&errors), destroyed_(&destroyed)
        {
        }

        void onAttach(lux::ecs::SystemAttach& attach) noexcept override
        {
            stale_writer_ = attach.commands();
        }

        void update(const lux::ecs::SystemFrame& frame) noexcept override
        {
            if (!queued_)
            {
                queued_ = true;
                if (frame.commands().push(ChainCommand{
                        frame.commands(), first_, final_, errors_}) !=
                    lux::ecs::ECommandResult::ACCEPTED)
                {
                    ++*errors_;
                }
                if (frame.commands().push(OwningCommand{
                        std::make_unique<Lifetime>(*destroyed_)}) !=
                    lux::ecs::ECommandResult::ACCEPTED)
                {
                    ++*errors_;
                }
            }
        }

        [[nodiscard]] bool removable() const noexcept override
        {
            return true;
        }

        [[nodiscard]] lux::ecs::WorldCommands staleWriter() const noexcept
        {
            return stale_writer_;
        }

      private:
        int* first_{};
        int* final_{};
        int* errors_{};
        int* destroyed_{};
        lux::ecs::WorldCommands stale_writer_;
        bool queued_{};
    };

    class AllocationSystem final : public lux::ecs::System
    {
      public:
        void update(const lux::ecs::SystemFrame& frame) noexcept override
        {
            if (frame.commands().push(Noop{}) != lux::ecs::ECommandResult::ACCEPTED)
                ++errors;
        }

        std::size_t errors{};
    };

    class CloseProvider final : public lux::ecs::System
    {
      public:
        explicit CloseProvider(std::vector<int>& order) noexcept : order_(&order) {}
        void update(const lux::ecs::SystemFrame&) noexcept override {}
        void requestClose() noexcept override { order_->push_back(1); }

      private:
        std::vector<int>* order_{};
    };

    class CloseConsumer final : public lux::ecs::System
    {
      public:
        explicit CloseConsumer(std::vector<int>& order) noexcept : order_(&order) {}
        void update(const lux::ecs::SystemFrame&) noexcept override {}
        void requestClose() noexcept override { order_->push_back(2); }

        std::span<const lux::cxx::TypeToken> requiredSystems() const noexcept override
        {
            static const lux::cxx::TypeToken required[]{
                lux::cxx::typeToken<CloseProvider>()};
            return required;
        }

      private:
        std::vector<int>* order_{};
    };
}

int main()
{
    using namespace lux::ecs;

    World world;
    Schedule schedule(world);

    {
        auto transaction = schedule.edit();
        assert(transaction);
        auto edit = std::move(*transaction);
        const auto first = edit.add(std::make_unique<Empty>());
        const auto duplicate = edit.add(std::make_unique<Empty>());
        assert(first && duplicate);
        const auto result = edit.commit();
        assert(!result && result.error().code == EScheduleError::DUPLICATE_SYSTEM);
        assert(schedule.get(first) == nullptr);
        assert(schedule.edit());
    }

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        const auto value = edit.add(std::make_unique<RequiresMissingType>());
        const auto result = edit.commit();
        assert(!result && result.error().code ==
            EScheduleError::MISSING_REQUIRED_SYSTEM);
        assert(schedule.get(value) == nullptr);
    }

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        (void)edit.add(std::make_unique<MissingSet>());
        const auto result = edit.commit();
        assert(!result && result.error().code == EScheduleError::MISSING_REQUIRED_SET);
    }

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        (void)edit.add(std::make_unique<CycleA>());
        (void)edit.add(std::make_unique<CycleB>());
        const auto result = edit.commit();
        assert(!result && result.error().code == EScheduleError::DEPENDENCY_CYCLE);
    }

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        (void)edit.add(
            std::make_unique<ContradictoryPre>(),
            SystemPhase::PreUpdate
        );
        (void)edit.add(std::make_unique<UpdateSet>(), SystemPhase::Update);
        const auto result = edit.commit();
        assert(!result && result.error().code ==
            EScheduleError::PHASE_ORDER_CONTRADICTION);
    }

    SystemHandle<RemovableProvider> provider_handle;
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        provider_handle = edit.add(std::make_unique<RemovableProvider>());
        assert(edit.add(std::make_unique<HardConsumer>()));
        assert(edit.commit());
    }
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        edit.remove(provider_handle);
        const auto result = edit.commit();
        assert(!result && result.error().code ==
            EScheduleError::HARD_DEPENDENT_EXISTS);
        assert(schedule.get(provider_handle) != nullptr);
    }

    auto world_edit_result = world.edit();
    auto world_edit = std::move(*world_edit_result);
    const Entity entity = world_edit.create();
    world_edit.emplace<Position>(entity, 3);
    world_edit = {};

    bool detached{};
    SystemHandle<FoldingObserver> observer_handle;
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        observer_handle = edit.add(std::make_unique<FoldingObserver>(detached));
        assert(edit.commit());
    }
    assert(world.find<Folded>(entity) != nullptr);
    assert(schedule.get(observer_handle)->errors() == 0);

    int first{};
    int final{};
    int errors{};
    int destroyed{};
    SystemHandle<CommandSystem> command_handle;
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        command_handle = edit.add(std::make_unique<CommandSystem>(
            first,
            final,
            errors,
            destroyed
        ));
        assert(edit.commit());
    }
    const WorldCommands stale = schedule.get(command_handle)->staleWriter();
    schedule.run(1.0F / 60.0F, 1);
    assert(first == 1 && final == 0 && destroyed == 1 && errors == 0);
    schedule.run(1.0F / 60.0F, 2);
    assert(first == 1 && final == 1 && destroyed == 1 && errors == 0);

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        assert(stale.push(Noop{}) == ECommandResult::ACCEPTED);
        edit.remove(command_handle);
        assert(edit.commit());
    }
    assert(schedule.get(command_handle) == nullptr);
    assert(stale.push(Noop{}) == ECommandResult::STALE_WRITER);
    assert(detail::ScheduleTestAccess::discardedCommands(schedule) == 1);

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        edit.remove(observer_handle);
        assert(edit.commit());
    }
    assert(detached);

    SystemHandle<AllocationSystem> allocation_handle;
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        allocation_handle = edit.add(std::make_unique<AllocationSystem>());
        assert(edit.commit());
    }
    schedule.run(1.0F / 60.0F, 3);
    const auto command_allocations_before =
        detail::ScheduleTestAccess::commandAllocationEvents(schedule);
    allocations.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    for (std::uint64_t tick = 4; tick != 104; ++tick)
        schedule.run(1.0F / 60.0F, tick);
    count_allocations.store(false, std::memory_order_relaxed);
    assert(allocations.load(std::memory_order_relaxed) == 0);
    assert(detail::ScheduleTestAccess::commandAllocationEvents(schedule) ==
        command_allocations_before);
    assert(schedule.get(allocation_handle)->errors == 0);

    std::vector<int> close_order;
    close_order.reserve(2);
    World close_world;
    {
        Schedule close_schedule(close_world);
        auto transaction = close_schedule.edit();
        auto edit = std::move(*transaction);
        assert(edit.add(std::make_unique<CloseConsumer>(close_order)));
        assert(edit.add(std::make_unique<CloseProvider>(close_order)));
        assert(edit.commit());
    }
    assert((close_order == std::vector<int>{2, 1}));
}
