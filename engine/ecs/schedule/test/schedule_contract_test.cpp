#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/schedule/detail/ScheduleTestAccess.hpp>

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>
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

    struct Velocity final
    {
        int value{};
    };

    struct Noop final
    {
        void apply(lux::ecs::WorldEdit&) noexcept {}
    };

    class Empty final : public lux::ecs::System
    {
      public:
        void update(lux::ecs::SystemFrame&) noexcept override {}
    };

    class StartFailure final : public lux::ecs::System
    {
      public:
        lux::cxx::expected<void, lux::ecs::SystemStartError>
        start(lux::ecs::SystemStart&) noexcept override
        {
            return lux::cxx::unexpected(lux::ecs::SystemStartError{});
        }

        void update(lux::ecs::SystemFrame&) noexcept override {}
    };

    class WrongAffinity final : public lux::ecs::System
    {
      public:
        using lux_thread_affine = std::true_type;

        [[nodiscard]] bool isOnAffinityThread() const noexcept
        {
            return false;
        }

        void update(lux::ecs::SystemFrame&) noexcept override {}
    };

    class StopSystem final : public lux::ecs::System
    {
      public:
        explicit StopSystem(std::vector<int>& order, int marker) noexcept
            : order_(&order), marker_(marker)
        {
        }

        void update(lux::ecs::SystemFrame&) noexcept override {}
        void requestStop() noexcept override
        {
            order_->push_back(marker_);
            stopped_ = true;
        }
        [[nodiscard]] bool stopped() const noexcept override { return stopped_; }

      private:
        std::vector<int>* order_{};
        int marker_{};
        bool stopped_{};
    };

    class PositionReader final : public lux::ecs::System
    {
      public:
        [[nodiscard]] lux::ecs::SystemAccess access() const noexcept override
        {
            return lux::ecs::access(
                lux::ecs::query<lux::ecs::Read<Position>>()
            );
        }
        void update(lux::ecs::SystemFrame&) noexcept override {}
    };

    class OtherPositionReader final : public lux::ecs::System
    {
      public:
        [[nodiscard]] lux::ecs::SystemAccess access() const noexcept override
        {
            return lux::ecs::access(
                lux::ecs::query<lux::ecs::Read<Position>>()
            );
        }
        void update(lux::ecs::SystemFrame&) noexcept override {}
    };

    class PositionWriter final : public lux::ecs::System
    {
      public:
        [[nodiscard]] lux::ecs::SystemAccess access() const noexcept override
        {
            return lux::ecs::access(
                lux::ecs::query<lux::ecs::Write<Position>>()
            );
        }
        void update(lux::ecs::SystemFrame&) noexcept override {}
    };

    class VelocityReader final : public lux::ecs::System
    {
      public:
        [[nodiscard]] lux::ecs::SystemAccess access() const noexcept override
        {
            return lux::ecs::access(
                lux::ecs::query<lux::ecs::Read<Velocity>>()
            );
        }
        void update(lux::ecs::SystemFrame&) noexcept override {}
    };

    struct FinalCommand final
    {
        int* applied{};
        void apply(lux::ecs::WorldEdit&) noexcept { ++*applied; }
    };

    struct ChainCommand final
    {
        lux::ecs::WorldCommands writer;
        int* first{};
        int* stale{};

        void apply(lux::ecs::WorldEdit&) noexcept
        {
            ++*first;
            if (writer.push(FinalCommand{first}) ==
                lux::ecs::ECommandResult::STALE_WRITER)
                ++*stale;
        }
    };

    class CommandSystem final : public lux::ecs::System
    {
      public:
        CommandSystem(
            lux::ecs::WorldCommands& escaped,
            int& first,
            int& stale
        ) noexcept
            : escaped_(&escaped), first_(&first), stale_(&stale)
        {
        }

        void update(lux::ecs::SystemFrame& frame) noexcept override
        {
            *escaped_ = frame.commands();
            assert(frame.commands().push(
                ChainCommand{frame.commands(), first_, stale_}
            ) == lux::ecs::ECommandResult::ACCEPTED);
        }

      private:
        lux::ecs::WorldCommands* escaped_{};
        int* first_{};
        int* stale_{};
    };

    class AllocationSystem final : public lux::ecs::System
    {
      public:
        void update(lux::ecs::SystemFrame& frame) noexcept override
        {
            if (frame.commands().push(Noop{}) !=
                lux::ecs::ECommandResult::ACCEPTED)
                ++errors;
        }
        std::size_t errors{};
    };

    class ReentrantSystem final : public lux::ecs::System
    {
      public:
        ReentrantSystem(lux::ecs::Schedule& schedule, bool& rejected) noexcept
            : schedule_(&schedule), rejected_(&rejected)
        {
        }

        void update(lux::ecs::SystemFrame&) noexcept override
        {
            const auto edit = schedule_->edit();
            *rejected_ = !edit &&
                edit.error().code == lux::ecs::EScheduleError::EXECUTING;
        }

      private:
        lux::ecs::Schedule* schedule_{};
        bool* rejected_{};
    };
}

int main()
{
    using namespace lux::ecs;

    World world;
    Schedule schedule(world);

    SystemHandle<Empty> first_empty;
    SystemHandle<Empty> second_empty;
    {
        auto transaction = schedule.edit();
        assert(transaction);
        auto edit = std::move(*transaction);
        first_empty = edit.add(std::make_unique<Empty>());
        second_empty = edit.add(std::make_unique<Empty>());
        assert(first_empty && second_empty && edit.commit());
    }

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        const auto failed = edit.add(std::make_unique<StartFailure>());
        const auto result = edit.commit();
        assert(!result && result.error().code ==
            EScheduleError::SYSTEM_START_FAILED);
        assert(!schedule.stopped(failed));
    }

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        (void)edit.add(std::make_unique<WrongAffinity>());
        const auto result = edit.commit();
        assert(!result && result.error().code ==
            EScheduleError::EXECUTION_AFFINITY_MISMATCH);
    }

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        const auto a = edit.add(std::make_unique<PositionReader>());
        const auto b = edit.add(std::make_unique<OtherPositionReader>());
        edit.before(a, b);
        edit.before(b, a);
        const auto result = edit.commit();
        assert(!result && result.error().code == EScheduleError::DEPENDENCY_CYCLE);
    }

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        const auto pre = edit.add(
            std::make_unique<PositionReader>(), SystemPhase::PreUpdate
        );
        const auto post = edit.add(
            std::make_unique<OtherPositionReader>(), SystemPhase::PostUpdate
        );
        edit.after(pre, post);
        const auto result = edit.commit();
        assert(!result && result.error().code ==
            EScheduleError::PHASE_ORDER_CONTRADICTION);
    }

    std::vector<int> stop_order;
    SystemHandle<StopSystem> provider;
    SystemHandle<StopSystem> consumer;
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        provider = edit.add(std::make_unique<StopSystem>(stop_order, 1));
        consumer = edit.add(std::make_unique<StopSystem>(stop_order, 2));
        edit.require(consumer, provider);
        assert(edit.commit());
    }

    std::vector<int> order_only_stop;
    SystemHandle<StopSystem> ordered_first;
    SystemHandle<StopSystem> ordered_second;
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        ordered_first = edit.add(
            std::make_unique<StopSystem>(order_only_stop, 3)
        );
        ordered_second = edit.add(
            std::make_unique<StopSystem>(order_only_stop, 4)
        );
        edit.before(ordered_first, ordered_second);
        assert(edit.commit());
    }
    assert(schedule.requestStop(ordered_second));
    assert(schedule.requestStop(ordered_first));
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        edit.remove(ordered_first);
        edit.remove(ordered_second);
        assert(edit.commit());
    }
    auto provider_stop = schedule.requestStop(provider);
    assert(!provider_stop && provider_stop.error().code ==
        EScheduleError::HARD_DEPENDENT_EXISTS);
    assert(schedule.requestStop(consumer));
    assert(schedule.requestStop(provider));
    assert((stop_order == std::vector<int>{2, 1}));
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        edit.remove(provider);
        const auto result = edit.commit();
        assert(!result && result.error().code ==
            EScheduleError::HARD_DEPENDENT_EXISTS);
    }
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        edit.remove(consumer);
        edit.remove(provider);
        assert(edit.commit());
    }

    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        const auto r1 = edit.add(std::make_unique<PositionReader>());
        const auto r2 = edit.add(std::make_unique<OtherPositionReader>());
        const auto w = edit.add(std::make_unique<PositionWriter>());
        const auto other = edit.add(std::make_unique<VelocityReader>());
        assert(r1 && r2 && w && other && edit.commit());
    }
    const auto plan = detail::ScheduleTestAccess::snapshot(schedule);
    bool shared_read_wave{};
    for (const auto& wave : plan.batches)
    {
        if (wave.size() >= 3)
            shared_read_wave = true;
    }
    assert(shared_read_wave);

    WorldCommands escaped;
    int first{};
    int stale{};
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        assert(edit.add(std::make_unique<CommandSystem>(
            escaped, first, stale
        )));
        assert(edit.commit());
    }
    schedule.run(1.0F / 60.0F, 1);
    assert(first == 1 && stale == 1);
    assert(escaped.push(Noop{}) == ECommandResult::STALE_WRITER);

    bool reentrant_rejected{};
    {
        auto transaction = schedule.edit();
        auto edit = std::move(*transaction);
        assert(edit.add(std::make_unique<ReentrantSystem>(
            schedule, reentrant_rejected
        )));
        assert(edit.add(std::make_unique<AllocationSystem>()));
        assert(edit.commit());
    }
    schedule.run(1.0F / 60.0F, 2);
    assert(reentrant_rejected);
    const auto command_allocations_before =
        detail::ScheduleTestAccess::commandAllocationEvents(schedule);
    allocations.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    for (std::uint64_t tick = 3; tick != 103; ++tick)
        schedule.run(1.0F / 60.0F, tick);
    count_allocations.store(false, std::memory_order_relaxed);
    assert(allocations.load(std::memory_order_relaxed) == 0);
    assert(detail::ScheduleTestAccess::commandAllocationEvents(schedule) ==
        command_allocations_before);

    World other_world;
    Schedule other(other_world);
    const auto cross = other.requestStop(first_empty);
    assert(!cross && cross.error().code == EScheduleError::INVALID_HANDLE);
}
