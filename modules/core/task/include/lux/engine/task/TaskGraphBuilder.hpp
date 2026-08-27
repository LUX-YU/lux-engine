#pragma once

#include <lux/engine/task/Task.hpp>
#include <lux/engine/task/TaskCallable.hpp>
#include <lux/engine/task/TaskGraph.hpp>
#include <lux/engine/task/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::task
{
    namespace detail
    {
        template <class Type>
        inline constexpr bool kTaskProperty =
            std::same_as<std::remove_cvref_t<Type>, TaskResourceAccess> ||
            std::same_as<std::remove_cvref_t<Type>, TaskResources> ||
            std::same_as<std::remove_cvref_t<Type>, TaskDependency> ||
            std::same_as<std::remove_cvref_t<Type>, TaskAffinity> ||
            std::same_as<std::remove_cvref_t<Type>, TaskLifetimePin>;
    }

    /**
     * Single-pass task graph construction.
     *
     * Explicit dependencies are properties of the task being defined. A task may
     * only depend on a TaskHandle already produced by this builder. This makes
     * insertion order a valid topological order by construction and eliminates
     * forward-reference patching and cycle detection from the public model.
     */
    class LUX_CORE_TASK_PUBLIC TaskGraphBuilder final
    {
    public:
        TaskGraphBuilder();
        ~TaskGraphBuilder() = default;

        TaskGraphBuilder(TaskGraphBuilder&& other) noexcept;
        TaskGraphBuilder& operator=(TaskGraphBuilder&& other) noexcept;

        TaskGraphBuilder(const TaskGraphBuilder&) = delete;
        TaskGraphBuilder& operator=(const TaskGraphBuilder&) = delete;

        /**
         * Properties come first and the callable is the final argument:
         *
         *   builder.add(
         *       task::dependsOn(previous),
         *       task::read(resource),
         *       task::write(other),
         *       [&]() noexcept { ...; });
         */
        template <class... Args>
        [[nodiscard]] lux::cxx::expected<TaskHandle, TaskGraphFailure>
        add(Args&&... args) noexcept
        {
            static_assert(sizeof...(Args) != 0U);
            auto arguments = std::forward_as_tuple(std::forward<Args>(args)...);
            return addTuple(
                std::move(arguments),
                std::make_index_sequence<sizeof...(Args) - 1U>{}
            );
        }

        [[nodiscard]] std::size_t taskCount() const noexcept
        {
            return tasks_.size();
        }

        /** Build the immutable execution graph and consume this builder. */
        [[nodiscard]] lux::cxx::expected<TaskGraph, TaskGraphFailure>
        build() && noexcept;

    private:
        struct PendingTask final
        {
            TaskCallable callable;
            std::vector<TaskResourceAccess> resources;
            std::vector<TaskHandle> dependencies;
            std::vector<std::shared_ptr<const void>> pins;
            ETaskAffinity affinity{ETaskAffinity::WORKER};
        };

        template <class Tuple, std::size_t... Index>
        [[nodiscard]] lux::cxx::expected<TaskHandle, TaskGraphFailure> addTuple(
            Tuple&& arguments,
            std::index_sequence<Index...>
        ) noexcept
        {
            constexpr std::size_t kCallableIndex = sizeof...(Index);
            using Callable = std::remove_cvref_t<decltype(
                std::get<kCallableIndex>(arguments)
            )>;

            static_assert((detail::kTaskProperty<decltype(
                std::get<Index>(arguments)
            )> && ...));
            static_assert(std::is_move_constructible_v<Callable>);
            static_assert(std::is_nothrow_invocable_r_v<void, const Callable&>);

            try
            {
                PendingTask pending;
                (collectProperty(
                    pending,
                    std::get<Index>(std::forward<Tuple>(arguments))
                ), ...);
                pending.callable = TaskCallable(
                    std::get<kCallableIndex>(std::forward<Tuple>(arguments))
                );
                return addPending(std::move(pending));
            }
            catch (...)
            {
                return lux::cxx::unexpected(TaskGraphFailure{
                    .code = ETaskGraphError::ALLOCATION_FAILURE
                });
            }
        }

        static void collectProperty(
            PendingTask& pending,
            TaskResourceAccess property
        )
        {
            pending.resources.push_back(property);
        }

        static void collectProperty(
            PendingTask& pending,
            TaskResources property
        )
        {
            pending.resources.insert(
                pending.resources.end(),
                std::make_move_iterator(property.values.begin()),
                std::make_move_iterator(property.values.end())
            );
        }

        static void collectProperty(
            PendingTask& pending,
            TaskDependency property
        )
        {
            pending.dependencies.push_back(property.task);
        }

        static void collectProperty(
            PendingTask& pending,
            TaskAffinity property
        ) noexcept
        {
            pending.affinity = property.value;
        }

        static void collectProperty(
            PendingTask& pending,
            TaskLifetimePin property
        )
        {
            if (property.value)
                pending.pins.push_back(std::move(property.value));
        }

        [[nodiscard]] lux::cxx::expected<TaskHandle, TaskGraphFailure>
        addPending(PendingTask pending) noexcept;

        [[nodiscard]] static TaskBuildId acquireBuildId() noexcept;

        TaskBuildId id_{};
        std::vector<PendingTask> tasks_;
    };
}
