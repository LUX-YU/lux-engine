#include <lux/engine/ecs/SystemExecution.hpp>

#include <lux/engine/ecs/ChangeSet.hpp>
#include <lux/engine/ecs/SystemContext.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/core/detail/CommandStorage.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemCompilation.hpp>
#include <lux/engine/ecs/system/detail/SystemContextAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemExecutionAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemExecutionTestAccess.hpp>

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

        class ExecutionLease final
        {
        public:
            ExecutionLease(World& world, SystemRegistry& systems) noexcept
                : world_(std::addressof(world)),
                  systems_(std::addressof(systems))
            {
            }

            ~ExecutionLease() noexcept
            {
                if (systems_ != nullptr)
                    detail::SystemRegistryAccess::releaseExecution(*systems_);
                if (world_ != nullptr)
                    detail::WorldExecutionAccess::release(*world_);
            }

            ExecutionLease(const ExecutionLease&) = delete;
            ExecutionLease& operator=(const ExecutionLease&) = delete;

        private:
            World* world_{};
            SystemRegistry* systems_{};
        };
    }

    struct SystemExecutionScratch::Impl final
    {
        std::vector<SystemTaskScratch> systems;
        lux::task::TaskExecutionScratch tasks;
        SystemRegistryId registry_id{};
        SystemRelationsId relations_id{};
        std::uint64_t registry_revision{};
        std::uint64_t relations_revision{};
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
        const CompiledSystemTaskGraph& compilation,
        std::size_t reserve_change_records
    ) noexcept
    {
        if (!impl_ || !compilation.impl_)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::INVALID_SYSTEM
            });
        }

        try
        {
            std::vector<SystemTaskScratch> prepared(
                compilation.impl_->scratch_layout.systems.size()
            );
            for (std::size_t index{}; index < prepared.size(); ++index)
            {
                auto lanes = prepared[index].changes.prepare(
                    compilation.impl_->scratch_layout
                        .systems[index].write_storages,
                    reserve_change_records
                );
                if (!lanes)
                    return lux::cxx::unexpected<SystemFailure>(lanes.error());
            }
            auto task_prepared = impl_->tasks.prepare(compilation.impl_->graph);
            if (!task_prepared)
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = task_prepared.error().code ==
                            lux::task::ETaskGraphError::ALLOCATION_FAILURE
                        ? ESystemError::ALLOCATION_FAILURE
                        : ESystemError::INVALID_SYSTEM
                });
            }
            impl_->systems = std::move(prepared);
            impl_->registry_id = compilation.impl_->registry_id;
            impl_->relations_id = compilation.impl_->relations_id;
            impl_->registry_revision = compilation.impl_->registry_revision;
            impl_->relations_revision = compilation.impl_->relations_revision;
            impl_->prepared_task_count = compilation.impl_->graph.taskCount();
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
        std::uint64_t result{};
        if (impl_)
            for (const auto& system : impl_->systems)
                result += system.changes.laneBindCount();
        return result;
    }

    std::uint64_t
    SystemExecutionScratch::journalStreamBindCount() const noexcept
    {
        std::uint64_t result{};
        if (impl_)
            for (const auto& system : impl_->systems)
                result += system.changes.journalStreamBindCount();
        return result;
    }

    std::uint64_t SystemExecutionScratch::recordAppendCount() const noexcept
    {
        std::uint64_t result{};
        if (impl_)
            for (const auto& system : impl_->systems)
                result += system.changes.recordAppendCount();
        return result;
    }

    std::uint64_t
    SystemExecutionScratch::perRecordLookupCount() const noexcept
    {
        std::uint64_t result{};
        if (impl_)
            for (const auto& system : impl_->systems)
                result += system.changes.perRecordLookupCount();
        return result;
    }

    lux::cxx::expected<void, SystemFailure> executeSystemTaskGraph(
        lux::task::TaskExecutionBackendRef backend,
        const CompiledSystemTaskGraph& compilation,
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
            const CompiledSystemTaskGraph& compilation,
            EcsExecutionContext& context
        ) noexcept
        {
            if (!compilation.impl_ || !context.scratch.impl_ ||
                compilation.impl_->registry_id != context.systems.id() ||
                compilation.impl_->registry_revision !=
                    context.systems.revision() ||
                compilation.impl_->relations_id != context.relations.id() ||
                compilation.impl_->relations_revision !=
                    context.relations.revision() ||
                context.scratch.impl_->registry_id !=
                    compilation.impl_->registry_id ||
                context.scratch.impl_->relations_id !=
                    compilation.impl_->relations_id ||
                context.scratch.impl_->registry_revision !=
                    compilation.impl_->registry_revision ||
                context.scratch.impl_->relations_revision !=
                    compilation.impl_->relations_revision ||
                context.scratch.impl_->systems.size() !=
                    compilation.impl_->scratch_layout.systems.size() ||
                context.scratch.impl_->prepared_task_count !=
                    compilation.impl_->graph.taskCount())
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::STALE_COMPILATION
                });
            }
            if (!WorldExecutionAccess::acquire(context.world))
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::WORLD_BUSY
                });
            }
            if (!SystemRegistryAccess::acquireExecution(context.systems))
            {
                WorldExecutionAccess::release(context.world);
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::WORLD_BUSY
                });
            }
            ExecutionLease lease(context.world, context.systems);
            lux::task::executeTaskGraph(
                backend,
                compilation.impl_->graph,
                std::addressof(context),
                context.scratch.impl_->tasks
            );
            return {};
        }

        void SystemExecutionAccess::invokeSystem(
            void* target,
            void* execution_context
        ) noexcept
        {
            const auto& task = *static_cast<const SystemTaskTarget*>(target);
            auto& execution = *static_cast<EcsExecutionContext*>(
                execution_context
            );
            require(
                task.scratch_index < execution.scratch.impl_->systems.size()
            );
            auto* record = SystemRegistryAccess::record(
                execution.systems,
                task.slot
            );
            require(
                record != nullptr && record->object != nullptr &&
                record->update == task.update &&
                record->affinity_valid == task.affinity_valid &&
                record->affinity_valid(record->object)
            );

            auto& scratch = execution.scratch.impl_->systems[
                task.scratch_index
            ];
            scratch.changes.reset();
            const WorldCommands commands = CommandShardAccess::begin(
                scratch.commands
            );
            auto context = SystemContextAccess::make(
                execution.world,
                scratch.changes,
                commands,
                execution.delta_seconds,
                execution.tick_index,
                task.allowed
            );
            task.update(record->object, context);
            CommandShardAccess::end(scratch.commands);
        }

        void SystemExecutionAccess::publishChanges(
            void* target,
            void* execution_context
        ) noexcept
        {
            const auto& publish = *static_cast<const SystemPublishTarget*>(
                target
            );
            auto& execution = *static_cast<EcsExecutionContext*>(
                execution_context
            );
            require(
                publish.scratch_index <
                execution.scratch.impl_->systems.size()
            );
            auto& changes = execution.scratch.impl_
                ->systems[publish.scratch_index].changes;
            if (changes.overflowed())
            {
                markWorldChangeHistoryLoss(execution.world);
                changes.reset();
                return;
            }
            (void)changes.publish(execution.world);
        }

        void SystemExecutionAccess::applyCommands(
            void*,
            void* execution_context
        ) noexcept
        {
            auto& execution = *static_cast<EcsExecutionContext*>(
                execution_context
            );
            WorldExecutionAccess::beginApplyingCommands(execution.world);
            auto mutation = WorldExecutionAccess::commandMutation(
                execution.world
            );
            for (auto& system : execution.scratch.impl_->systems)
                CommandShardAccess::apply(system.commands, mutation);
            mutation = {};
            WorldExecutionAccess::resume(execution.world);
        }

        void SystemExecutionTestAccess::failNextCommandPush(
            const CompiledSystemTaskGraph& compilation,
            SystemExecutionScratch& scratch,
            SystemId id
        ) noexcept
        {
            require(compilation.impl_ && scratch.impl_);
            require(id.owner == compilation.impl_->registry_id);
            for (const auto& target : compilation.impl_->system_tasks)
            {
                if (target.slot != id.slot)
                    continue;
                require(target.scratch_index < scratch.impl_->systems.size());
                scratch.impl_->systems[target.scratch_index]
                    .commands.failNextPushForTest();
                return;
            }
            contractFailure();
        }
    }
}
