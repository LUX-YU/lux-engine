#pragma once

#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemTaskResources.hpp>
#include <lux/engine/ecs/WorldTaskResources.hpp>
#include <lux/engine/ecs/core/detail/WorldTaskResourceTestAccess.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace lux::ecs::testing
{
    /** Test-only explicit, registration-ordered TaskGraph composition. */
    class EcsTaskTestRig final
    {
      public:
        explicit EcsTaskTestRig(World& world) : world_(&world) {}

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

            auto changes = std::make_shared<WorldChangeBatch>();
            assert(changes->prepare(Type::TaskAccess.writeStorages()));
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
                            *world_,
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
                            *world_,
                            *changes,
                            scope.commands()
                        );
                    }
                );
            assert(update);
            const auto publish = builder_.add(
                task::dependsOn(*update),
                worldChangesWrite(),
                [this, changes]() noexcept
                {
                    (void)changes->publish(*world_);
                }
            );
            assert(publish);
            entries_.push_back(Entry{*id, *publish});
            return *id;
        }

        [[nodiscard]] bool compile()
        {
            if (!commands_.prepare(entries_.size()))
                return false;

            lux::cxx::expected<task::TaskHandle, task::TaskGraphFailure> apply =
                entries_.empty()
                ? builder_.add(
                    task::on(task::ETaskAffinity::CALLER_THREAD),
                    worldCommandsWrite(),
                    [this]() noexcept
                    {
                        applyWorldCommands(*world_, commands_);
                    }
                )
                : builder_.add(
                    task::dependsOn(entries_.back().publish),
                    task::on(task::ETaskAffinity::CALLER_THREAD),
                    worldCommandsWrite(),
                    [this]() noexcept
                    {
                        applyWorldCommands(*world_, commands_);
                    }
                );
            if (!apply)
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
            auto lease = world_->beginTaskExecution();
            if (!lease)
                return false;
            return static_cast<bool>(executor_->execute(*graph_));
        }

        void failNextCommandPush(SystemId id) noexcept
        {
            const auto* entry = find(id);
            assert(entry != nullptr);
            const auto index = static_cast<std::size_t>(
                entry - entries_.data()
            );
            detail::WorldTaskResourceTestAccess::failNextPush(
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

        World* world_{};
        SystemRegistry systems_;
        task::TaskGraphBuilder builder_;
        std::vector<Entry> entries_;
        WorldCommandBatch commands_;
        std::unique_ptr<task::TaskGraph> graph_;
        std::unique_ptr<task::TaskExecutor> executor_;
    };
}
