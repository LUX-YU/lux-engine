#pragma once

#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/ecs/SystemTaskResources.hpp>
#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
#include <lux/engine/simulation/ecs/core/detail/EcsTaskResourceTestAccess.hpp>
#include <lux/engine/simulation/SimulationExecution.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace lux::simulation::ecs::testing
{
    struct EcsTaskTestRigCapacity final
    {
        EcsTaskTestRigCapacity() = delete;

        constexpr EcsTaskTestRigCapacity(
            std::size_t change_records_per_lane,
            EcsCommandProducerCapacity command_producer
        ) noexcept
            : change_records_per_lane(change_records_per_lane),
              command_producer(command_producer)
        {
        }

        std::size_t change_records_per_lane;
        EcsCommandProducerCapacity command_producer;
    };

    /** Test-only explicit, registration-ordered TaskGraph composition. */
    class EcsTaskTestRig final
    {
      public:
        EcsTaskTestRig(
            EcsState& state,
            EcsChangeHistoryBudget history_budget,
            EcsTaskTestRigCapacity capacity
        )
            : owned_journal_(std::make_unique<EcsChangeJournal>(
                  history_budget
              )),
              state_(&state),
              journal_(owned_journal_.get()),
              capacity_(capacity)
        {
        }

        EcsTaskTestRig(
            EcsState& state,
            EcsChangeJournal& journal,
            EcsTaskTestRigCapacity capacity
        ) noexcept
            : state_(&state), journal_(&journal), capacity_(capacity)
        {
        }

        template <class Type, class... Args>
        [[nodiscard]] SystemId add(Args&&... args)
        {
            const auto id = systems_.emplace<Type>(
                std::forward<Args>(args)...
            );
            assert(id);
            auto retained = systems_.retain<Type>(*id);
            assert(retained);
            auto system = std::move(*retained);

            auto changes = std::make_shared<EcsChangeBatch>();
            assert(changes->prepare(
                Type::TaskAccess.writeStorages(),
                capacity_.change_records_per_lane
            ));
            const std::size_t producer = entries_.size();

            lux::cxx::expected<task::TaskHandle, task::TaskGraphFailure> update =
                entries_.empty()
                ? builder_.add(
                    systemTaskResources<Type>(),
                    [this, system, changes, producer]() noexcept
                    {
                        auto recording = commands_.begin(producer);
                        detail::require(static_cast<bool>(recording));
                        auto scope = std::move(*recording);
                        system->invokeTask(
                            *state_,
                            *journal_,
                            *changes,
                            scope.commands()
                        );
                    }
                )
                : builder_.add(
                    task::dependsOn(entries_.back().publish),
                    systemTaskResources<Type>(),
                    [this, system, changes, producer]() noexcept
                    {
                        auto recording = commands_.begin(producer);
                        detail::require(static_cast<bool>(recording));
                        auto scope = std::move(*recording);
                        system->invokeTask(
                            *state_,
                            *journal_,
                            *changes,
                            scope.commands()
                        );
                    }
                );
            assert(update);
            const auto publish = builder_.add(
                task::dependsOn(*update),
                ecsChangesWrite(),
                [this, changes]() noexcept
                {
                    (void)changes->publish(*journal_);
                }
            );
            assert(publish);
            entries_.push_back(Entry{*id, *publish});
            return *id;
        }

        [[nodiscard]] bool compile()
        {
            std::vector<EcsCommandProducerCapacity> capacities(
                entries_.size(),
                capacity_.command_producer
            );
            if (!commands_.prepare(capacities))
                return false;

            auto built = std::move(builder_).build();
            if (!built)
                return false;
            graph_ = std::make_unique<task::TaskGraph>(std::move(*built));
            executor_ = std::make_unique<task::TaskExecutor>(
                task::TaskExecutorConfig{0U, graph_->taskCount()}
            );
            return true;
        }

        [[nodiscard]] bool run(float = 0.0F, std::uint64_t = 0U)
        {
            if (!graph_ || !executor_)
                return false;
            return static_cast<bool>(executeSimulationStep(
                *executor_,
                *graph_,
                *state_,
                *journal_,
                commands_
            ));
        }

        [[nodiscard]] auto mutate() noexcept
        {
            return beginSimulationEcsMutation(*state_, *journal_);
        }

        [[nodiscard]] EcsChangeJournal& journal() noexcept
        {
            return *journal_;
        }

        void failNextCommandPush(SystemId id) noexcept
        {
            const auto* entry = find(id);
            assert(entry != nullptr);
            const auto index = static_cast<std::size_t>(
                entry - entries_.data()
            );
            detail::EcsTaskResourceTestAccess::failNextPush(
                commands_,
                index
            );
        }

        template <class Type>
        [[nodiscard]] Type& system(SystemId id)
        {
            auto retained = systems_.retain<Type>(id);
            assert(retained);
            return retained->get();
        }

      private:
        struct Entry final
        {
            SystemId system;
            task::TaskHandle publish;
        };

        [[nodiscard]] const Entry* find(SystemId id) const noexcept
        {
            for (const auto& entry : entries_)
            {
                if (entry.system == id)
                    return &entry;
            }
            return nullptr;
        }

        std::unique_ptr<EcsChangeJournal> owned_journal_;
        EcsState* state_{};
        EcsChangeJournal* journal_{};
        SystemRegistry systems_;
        task::TaskGraphBuilder builder_;
        std::vector<Entry> entries_;
        EcsCommandBatch commands_;
        EcsTaskTestRigCapacity capacity_;
        std::unique_ptr<task::TaskGraph> graph_;
        std::unique_ptr<task::TaskExecutor> executor_;
    };
}
