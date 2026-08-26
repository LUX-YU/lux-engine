#include <lux/engine/ecs/SystemContext.hpp>
#include <lux/engine/ecs/SystemExecution.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>

#include <cassert>
#include <cstddef>
#include <span>

namespace
{
    struct Position final
    {
        int value{};
    };

    struct Applied final
    {
    };

    struct Probe final
    {
        std::size_t current_changes{};
        std::size_t resyncs{};
    };

    struct AddApplied final
    {
        lux::ecs::Entity entity{lux::ecs::NullEntity};

        void apply(lux::ecs::WorldMutation& mutation) noexcept
        {
            mutation.emplace<Applied>(entity);
        }
    };

    class WriterSystem final
        : public lux::ecs::StaticSystemAccess<lux::ecs::Write<Position>>
    {
    public:
        WriterSystem(lux::ecs::Entity entity, Probe& probe) noexcept
            : entity_(entity), probe_(&probe)
        {
        }

        void update(lux::ecs::SystemContext& context) noexcept
        {
            for (auto [entity, position] :
                 context.query<lux::ecs::Write<Position>>())
            {
                assert(entity == entity_);
                ++position.value;
            }
            if (!command_sent_)
            {
                assert(
                    context.commands().push(AddApplied{entity_}) ==
                    lux::ecs::ECommandResult::ACCEPTED
                );
                command_sent_ = true;
            }
        }

    private:
        lux::ecs::Entity entity_{lux::ecs::NullEntity};
        Probe* probe_{};
        bool command_sent_{};
    };

    class ReaderSystem final
        : public lux::ecs::StaticSystemAccess<
            lux::ecs::Read<Position>,
            lux::ecs::ExternalWrite<Probe>
        >
    {
    public:
        explicit ReaderSystem(Probe& probe) noexcept : probe_(&probe) {}

        void update(lux::ecs::SystemContext& context) noexcept
        {
            auto changes = context.changes(cursor_);
            if (changes.status() == lux::ecs::EChangeReadStatus::RESYNC_REQUIRED)
            {
                ++probe_->resyncs;
                return;
            }
            probe_->current_changes += changes.size();
        }

    private:
        Probe* probe_{};
        lux::ecs::ChangeCursor<Position> cursor_;
    };

    class NoOpSystem final
        : public lux::ecs::StaticSystemAccess<>
    {
    public:
        void update(lux::ecs::SystemContext&) noexcept {}
    };

    struct AffinityProbe final
    {
        std::size_t worker_tasks{};
        std::size_t owner_tasks{};
    };

    void submitTask(
        void* state,
        lux::task::TaskSubmission&& submission
    ) noexcept
    {
        auto& probe = *static_cast<AffinityProbe*>(state);
        if (submission.affinity() == lux::task::ETaskAffinity::OWNER_THREAD)
            ++probe.owner_tasks;
        else
            ++probe.worker_tasks;
        std::move(submission).run();
    }
}

int main()
{
    lux::ecs::World world{
        lux::ecs::WorldConfig{{256U * 1024U, 32U * 1024U * 1024U}}
    };
    auto mutation_result = world.mutate();
    assert(mutation_result);
    auto mutation = std::move(*mutation_result);
    const auto entity = mutation.create();
    mutation.emplace<Position>(entity);
    mutation = {};

    Probe probe;
    lux::ecs::SystemRegistry systems;
    const auto writer = systems.emplace<WriterSystem>(entity, probe);
    const auto reader = systems.emplace<ReaderSystem>(probe);
    assert(writer && reader);

    lux::ecs::SystemRelations relations;
    assert(relations.before(*writer, *reader));

    auto compilation_result = lux::ecs::compileSystemTaskGraph(
        systems,
        relations
    );
    assert(compilation_result);
    auto compilation = std::move(*compilation_result);
    assert(compilation.sourceRegistryRevision() == systems.revision());
    assert(compilation.sourceRelationsRevision() == relations.revision());
    assert(compilation.taskCount() == 4U);
    assert(compilation.dependencyCount() == 3U);

    lux::ecs::SystemExecutionScratch scratch;
    assert(scratch.prepare(compilation, 4U));

    AffinityProbe affinity;
    {
        lux::ecs::EcsExecutionContext context{
            world, systems, relations, scratch, 1.0F / 60.0F, 1U
        };
        assert(lux::ecs::executeSystemTaskGraph(
            lux::task::TaskExecutionBackendRef{&affinity, &submitTask},
            compilation,
            context
        ));
    }
    assert(affinity.worker_tasks == 2U);
    assert(affinity.owner_tasks == 2U);
    assert(probe.resyncs == 1U);
    assert(world.get<Position>(entity).value == 1);
    assert(world.find<Applied>(entity) != nullptr);

    {
        lux::ecs::EcsExecutionContext context{
            world, systems, relations, scratch, 1.0F / 60.0F, 2U
        };
        assert(lux::ecs::executeSystemTaskGraph(
            lux::task::referenceTaskExecutionBackend(),
            compilation,
            context
        ));
    }
    assert(world.get<Position>(entity).value == 2);
    assert(probe.current_changes == 1U);
    assert(scratch.laneBindCount() == 2U);
    assert(scratch.journalStreamBindCount() == 2U);
    assert(scratch.recordAppendCount() == 2U);
    assert(scratch.perRecordLookupCount() == 0U);

    assert(relations.after(*writer, *reader));
    {
        lux::ecs::EcsExecutionContext context{
            world, systems, relations, scratch, 0.0F, 3U
        };
        const auto stale = lux::ecs::executeSystemTaskGraph(
            lux::task::referenceTaskExecutionBackend(),
            compilation,
            context
        );
        assert(!stale);
        assert(stale.error().code == lux::ecs::ESystemError::STALE_COMPILATION);
    }

    const auto added = systems.emplace<NoOpSystem>();
    assert(added);
    {
        lux::ecs::EcsExecutionContext context{
            world, systems, relations, scratch, 0.0F, 4U
        };
        const auto stale = lux::ecs::executeSystemTaskGraph(
            lux::task::referenceTaskExecutionBackend(),
            compilation,
            context
        );
        assert(!stale);
        assert(stale.error().code == lux::ecs::ESystemError::STALE_COMPILATION);
    }

    {
        lux::ecs::SystemRegistry independent_systems;
        const auto first = independent_systems.emplace<NoOpSystem>();
        const auto second = independent_systems.emplace<NoOpSystem>();
        assert(first && second);
        lux::ecs::SystemRelations independent_relations;
        auto independent = lux::ecs::compileSystemTaskGraph(
            independent_systems,
            independent_relations
        );
        assert(independent);
        assert(independent->taskCount() == 3U);
        assert(independent->dependencyCount() == 2U);
    }

    lux::ecs::SystemRegistry cyclic_systems;
    const auto first = cyclic_systems.emplace<NoOpSystem>();
    const auto second = cyclic_systems.emplace<NoOpSystem>();
    assert(first && second);
    lux::ecs::SystemRelations cyclic_relations;
    assert(cyclic_relations.before(*first, *second));
    assert(cyclic_relations.before(*second, *first));
    const auto cyclic = lux::ecs::compileSystemTaskGraph(
        cyclic_systems,
        cyclic_relations
    );
    assert(!cyclic);
    assert(cyclic.error().code == lux::ecs::ESystemError::RELATION_CYCLE);
}
