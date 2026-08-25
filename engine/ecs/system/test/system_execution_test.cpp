#include <lux/engine/ecs/SystemContext.hpp>
#include <lux/engine/ecs/SystemExecution.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>

#include <cassert>
#include <cstddef>

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
        bool started{};
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

        [[nodiscard]] lux::cxx::expected<
            void,
            lux::ecs::SystemStartError
        > start(lux::ecs::SystemStart&) noexcept
        {
            probe_->started = true;
            return {};
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
}

int main()
{
    lux::ecs::World world;
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

    lux::ecs::SystemRelations relations(systems);
    assert(relations.before(*writer, *reader));

    lux::ecs::SystemTaskGraphCompiler compiler;
    auto compilation_result = compiler.compile(systems, relations);
    assert(compilation_result);
    auto compilation = std::move(*compilation_result);
    assert(compilation.registry_revision == systems.revision());
    assert(compilation.relations_revision == relations.revision());
    assert(compilation.graph.taskCount() == 4U);

    lux::ecs::SystemExecutionScratch scratch;
    assert(scratch.prepare(compilation, 4U));

    {
        lux::ecs::EcsExecutionContext context(
            world,
            systems,
            scratch,
            1.0F / 60.0F,
            1U
        );
        assert(lux::ecs::executeSystemTaskGraph(
            lux::task::referenceTaskExecutionBackend(),
            compilation,
            context
        ));
    }
    assert(probe.started);
    assert(probe.resyncs == 1U);
    assert(world.get<Position>(entity).value == 1);
    assert(world.find<Applied>(entity) != nullptr);

    {
        lux::ecs::EcsExecutionContext context(
            world,
            systems,
            scratch,
            1.0F / 60.0F,
            2U
        );
        assert(lux::ecs::executeSystemTaskGraph(
            lux::task::referenceTaskExecutionBackend(),
            compilation,
            context
        ));
    }
    assert(world.get<Position>(entity).value == 2);
    assert(probe.current_changes == 1U);
    assert(scratch.laneBindCount() == 2U);
    assert(scratch.perRecordLookupCount() == 0U);

    const auto added = systems.emplace<NoOpSystem>();
    assert(added);
    {
        lux::ecs::EcsExecutionContext context(
            world,
            systems,
            scratch,
            0.0F,
            3U
        );
        const auto stale = lux::ecs::executeSystemTaskGraph(
            lux::task::referenceTaskExecutionBackend(),
            compilation,
            context
        );
        assert(!stale);
        assert(stale.error().code == lux::ecs::ESystemError::STALE_COMPILATION);
    }

    lux::ecs::SystemRegistry cyclic_systems;
    const auto first = cyclic_systems.emplace<NoOpSystem>();
    const auto second = cyclic_systems.emplace<NoOpSystem>();
    assert(first && second);
    lux::ecs::SystemRelations cyclic_relations(cyclic_systems);
    assert(cyclic_relations.before(*first, *second));
    assert(cyclic_relations.before(*second, *first));
    const auto cyclic = compiler.compile(cyclic_systems, cyclic_relations);
    assert(!cyclic);
    assert(cyclic.error().code == lux::ecs::ESystemError::RELATION_CYCLE);
}
