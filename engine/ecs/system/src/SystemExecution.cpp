#include <lux/engine/ecs/SystemExecution.hpp>

#include <lux/engine/ecs/ChangeSet.hpp>
#include <lux/engine/ecs/SystemContext.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>
#include <lux/engine/ecs/core/detail/CommandStorage.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemContextAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemExecutionAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemExecutionTestAccess.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        struct SystemTaskScratch final
        {
            ChangeSet changes;
            detail::CommandShard commands;
        };
    }

    struct SystemExecutionScratch::Impl final
    {
        std::vector<std::unique_ptr<SystemTaskScratch>> systems;
        lux::task::TaskExecutionScratch tasks;
        std::size_t prepared_task_count{};
    };

    SystemExecutionScratch::SystemExecutionScratch()
        : impl_(std::make_unique<Impl>())
    {
    }

    SystemExecutionScratch::~SystemExecutionScratch() = default;
    SystemExecutionScratch::SystemExecutionScratch(
        SystemExecutionScratch&&
    ) noexcept = default;
    SystemExecutionScratch& SystemExecutionScratch::operator=(
        SystemExecutionScratch&&
    ) noexcept = default;

    lux::cxx::expected<void, SystemFailure> SystemExecutionScratch::prepare(
        const SystemTaskGraphCompilation& compilation,
        std::size_t reserve_change_records
    ) noexcept
    {
        if (!impl_ || !compilation.state_)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::INVALID_SYSTEM
            });
        }

        try
        {
            impl_->systems.clear();
            impl_->systems.reserve(compilation.scratch_layout.systems.size());
            for (const auto& layout : compilation.scratch_layout.systems)
            {
                auto scratch = std::make_unique<SystemTaskScratch>();
                auto prepared = scratch->changes.prepare(
                    layout.write_storages,
                    reserve_change_records
                );
                if (!prepared)
                    return lux::cxx::unexpected<SystemFailure>(prepared.error());
                scratch->commands.reserve(8U);
                impl_->systems.push_back(std::move(scratch));
            }
            auto task_prepared = impl_->tasks.prepare(compilation.graph);
            if (!task_prepared)
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = task_prepared.error().code ==
                            lux::task::ETaskGraphError::ALLOCATION_FAILURE
                        ? ESystemError::ALLOCATION_FAILURE
                        : ESystemError::INVALID_SYSTEM
                });
            }
            impl_->prepared_task_count = compilation.graph.taskCount();
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::ALLOCATION_FAILURE
            });
        }
    }

    std::size_t SystemExecutionScratch::systemCapacity() const noexcept
    {
        return impl_ ? impl_->systems.capacity() : 0U;
    }

    std::uint64_t SystemExecutionScratch::laneBindCount() const noexcept
    {
        if (!impl_)
            return 0U;
        std::uint64_t result{};
        for (const auto& system : impl_->systems)
            result += system->changes.laneBindCount();
        return result;
    }

    std::uint64_t SystemExecutionScratch::perRecordLookupCount() const noexcept
    {
        if (!impl_)
            return 0U;
        std::uint64_t result{};
        for (const auto& system : impl_->systems)
            result += system->changes.perRecordLookupCount();
        return result;
    }

    EcsExecutionContext::EcsExecutionContext(
        World& world,
        const SystemRegistry& systems,
        SystemExecutionScratch& scratch,
        float delta_seconds,
        std::uint64_t tick_index
    ) noexcept
        : world_(std::addressof(world)),
          systems_(std::addressof(systems)),
          scratch_(std::addressof(scratch)),
          delta_seconds_(delta_seconds),
          tick_index_(tick_index)
    {
    }

    EcsExecutionContext::~EcsExecutionContext() noexcept
    {
        if (executing_)
            detail::WorldExecutionAccess::release(*world_);
    }

    lux::cxx::expected<void, SystemFailure> executeSystemTaskGraph(
        lux::task::TaskExecutionBackendRef backend,
        const SystemTaskGraphCompilation& compilation,
        EcsExecutionContext& context
    ) noexcept
    {
        return detail::SystemExecutionAccess::execute(
            backend,
            compilation,
            context
        );
    }

    namespace detail
    {
        lux::cxx::expected<void, SystemFailure> SystemExecutionAccess::execute(
            lux::task::TaskExecutionBackendRef backend,
            const SystemTaskGraphCompilation& compilation,
            EcsExecutionContext& context
        ) noexcept
        {
            if (!compilation.state_ || !context.world_ || !context.systems_ ||
                !context.scratch_ || !context.scratch_->impl_ ||
                compilation.state_->registry != context.systems_ ||
                compilation.registry_revision != context.systems_->revision() ||
                !compilation.state_->relations ||
                compilation.relations_revision !=
                    compilation.state_->relations->revision() ||
                context.scratch_->impl_->systems.size() !=
                    compilation.scratch_layout.systems.size() ||
                context.scratch_->impl_->prepared_task_count !=
                    compilation.graph.taskCount())
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::STALE_COMPILATION
                });
            }
            if (!WorldExecutionAccess::acquire(*context.world_))
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::WORLD_BUSY
                });
            }
            context.executing_ = true;

            SystemStart start = context.startContext();
            for (const auto& task : compilation.state_->system_tasks)
            {
                if (!task.system->affinity_valid(task.system->object.get()))
                    contractFailure();
                if (task.system->started)
                    continue;
                auto started = task.system->start(
                    task.system->object.get(),
                    start
                );
                if (!started)
                {
                    WorldExecutionAccess::release(*context.world_);
                    context.executing_ = false;
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = ESystemError::START_FAILED
                    });
                }
                task.system->started = true;
            }

            lux::task::executeTaskGraph(
                backend,
                compilation.graph,
                std::addressof(context),
                context.scratch_->impl_->tasks
            );
            require(!context.executing_);
            return {};
        }

        void SystemExecutionAccess::invokeSystem(
            void* target,
            void* execution_context
        ) noexcept
        {
            auto& task = *static_cast<SystemTaskTarget*>(target);
            auto& execution = *static_cast<EcsExecutionContext*>(
                execution_context
            );
            require(execution.executing_);
            require(task.scratch_index < execution.scratch_->impl_->systems.size());
            auto& scratch = *execution.scratch_->impl_->systems[task.scratch_index];
            scratch.changes.reset();
            const WorldCommands commands = CommandShardAccess::begin(
                scratch.commands
            );
            auto context = SystemContextAccess::make(
                *execution.world_,
                scratch.changes,
                commands,
                execution.delta_seconds_,
                execution.tick_index_,
                task.system->access.components
            );
            task.system->update(task.system->object.get(), context);
            CommandShardAccess::end(scratch.commands);
        }

        void SystemExecutionAccess::publishChanges(
            void* target,
            void* execution_context
        ) noexcept
        {
            auto& publish = *static_cast<SystemPublishTarget*>(target);
            auto& execution = *static_cast<EcsExecutionContext*>(
                execution_context
            );
            require(execution.executing_);

            bool overflow{};
            for (const std::size_t index : publish.scratch_indices)
            {
                require(index < execution.scratch_->impl_->systems.size());
                overflow = overflow || execution.scratch_->impl_
                    ->systems[index]->changes.overflowed();
            }
            if (overflow)
            {
                markWorldChangeHistoryLoss(*execution.world_);
                for (const std::size_t index : publish.scratch_indices)
                    execution.scratch_->impl_->systems[index]->changes.reset();
                return;
            }
            for (const std::size_t index : publish.scratch_indices)
            {
                (void)execution.scratch_->impl_
                    ->systems[index]->changes.publish(*execution.world_);
            }
        }

        void SystemExecutionAccess::applyCommands(
            void*,
            void* execution_context
        ) noexcept
        {
            auto& execution = *static_cast<EcsExecutionContext*>(
                execution_context
            );
            require(execution.executing_);
            WorldExecutionAccess::beginApplyingCommands(*execution.world_);
            auto mutation = WorldExecutionAccess::commandMutation(
                *execution.world_
            );
            for (auto& system : execution.scratch_->impl_->systems)
                CommandShardAccess::apply(system->commands, mutation);
            mutation = {};
            WorldExecutionAccess::release(*execution.world_);
            execution.executing_ = false;
        }

        void SystemExecutionTestAccess::failNextCommandPush(
            const SystemTaskGraphCompilation& compilation,
            SystemExecutionScratch& scratch,
            SystemId id
        ) noexcept
        {
            require(compilation.state_ && scratch.impl_);
            for (const auto& target : compilation.state_->system_tasks)
            {
                if (target.id != id)
                    continue;
                require(target.scratch_index < scratch.impl_->systems.size());
                scratch.impl_->systems[target.scratch_index]
                    ->commands.failNextPushForTest();
                return;
            }
            contractFailure();
        }
    }
}
