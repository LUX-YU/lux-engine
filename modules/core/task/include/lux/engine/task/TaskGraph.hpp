#pragma once

#include <lux/engine/task/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <algorithm>
#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::task
{
    inline constexpr std::uint32_t InvalidTaskIndex =
        std::numeric_limits<std::uint32_t>::max();

    struct TaskId final
    {
        std::uint32_t index{InvalidTaskIndex};

        [[nodiscard]] constexpr bool isValid() const noexcept
        {
            return index != InvalidTaskIndex;
        }

        [[nodiscard]] constexpr bool operator==(const TaskId&) const noexcept =
            default;
    };

    struct TaskResourceKey final
    {
        std::uint64_t domain{};
        std::uint64_t value{};

        [[nodiscard]] constexpr bool operator==(
            const TaskResourceKey&
        ) const noexcept = default;

        [[nodiscard]] constexpr auto operator<=>(
            const TaskResourceKey&
        ) const noexcept = default;
    };

    enum class ETaskResourceAccess : std::uint8_t
    {
        READ,
        WRITE,
    };

    enum class ETaskAffinity : std::uint8_t
    {
        WORKER,
        CALLER_THREAD,
    };

    struct TaskResourceAccess final
    {
        TaskResourceKey key{};
        ETaskResourceAccess access{ETaskResourceAccess::READ};
    };

    struct TaskResourceList final
    {
        std::vector<TaskResourceAccess> values;
    };

    struct TaskAffinityProperty final
    {
        ETaskAffinity value{ETaskAffinity::WORKER};
    };

    struct TaskLifetimePin final
    {
        std::shared_ptr<const void> value;
    };

    [[nodiscard]] constexpr TaskResourceAccess read(TaskResourceKey key) noexcept
    {
        return {key, ETaskResourceAccess::READ};
    }

    [[nodiscard]] constexpr TaskResourceAccess write(TaskResourceKey key) noexcept
    {
        return {key, ETaskResourceAccess::WRITE};
    }

    [[nodiscard]] inline TaskResourceList resources(
        std::span<const TaskResourceAccess> accesses
    )
    {
        return {std::vector<TaskResourceAccess>(
            accesses.begin(),
            accesses.end()
        )};
    }

    template <class Range>
        requires requires(const Range& range)
        {
            std::span<const TaskResourceAccess>(range);
        }
    [[nodiscard]] TaskResourceList resources(const Range& accesses)
    {
        return resources(std::span<const TaskResourceAccess>(accesses));
    }

    [[nodiscard]] constexpr TaskAffinityProperty on(
        ETaskAffinity affinity
    ) noexcept
    {
        return {affinity};
    }

    [[nodiscard]] inline TaskLifetimePin keepAlive(
        std::shared_ptr<const void> lifetime
    ) noexcept
    {
        return {std::move(lifetime)};
    }

    enum class ETaskGraphError : std::uint8_t
    {
        INVALID_TASK,
        INVALID_CALLABLE,
        INVALID_RESOURCE,
        DUPLICATE_RESOURCE,
        DUPLICATE_DEPENDENCY,
        DEPENDENCY_CYCLE,
        ALLOCATION_FAILURE,
    };

    struct TaskGraphFailure final
    {
        ETaskGraphError code{ETaskGraphError::INVALID_TASK};
        TaskId task{};
        TaskId related{};
        TaskResourceKey resource{};
    };

    enum class ETaskRunError : std::uint8_t
    {
        NOT_PREPARED,
        ALREADY_RUNNING,
        ALLOCATION_FAILURE,
    };

    struct TaskRunFailure final
    {
        ETaskRunError code{ETaskRunError::NOT_PREPARED};
    };

    namespace detail
    {
        [[noreturn]] LUX_CORE_TASK_PUBLIC void taskContractFailure() noexcept;

        struct LUX_CORE_TASK_PUBLIC ErasedCallable final
        {
            void* object{};
            void (*invoke)(const void*) noexcept{};
            void (*destroy)(void*) noexcept{};

            ErasedCallable() noexcept = default;
            ErasedCallable(ErasedCallable&& other) noexcept;
            ErasedCallable& operator=(ErasedCallable&& other) noexcept;
            ~ErasedCallable() noexcept;
            ErasedCallable(const ErasedCallable&) = delete;
            ErasedCallable& operator=(const ErasedCallable&) = delete;

            void reset() noexcept;
        };

        struct TaskDefinition final
        {
            ErasedCallable callable;
            std::vector<TaskResourceAccess> resources;
            std::vector<std::shared_ptr<const void>> pins;
            ETaskAffinity affinity{ETaskAffinity::WORKER};
        };

        struct Edge final
        {
            std::uint32_t before{};
            std::uint32_t after{};

            [[nodiscard]] bool operator==(const Edge&) const noexcept = default;
            [[nodiscard]] auto operator<=>(const Edge&) const noexcept = default;
        };

        template <class Type>
        inline constexpr bool kTaskProperty =
            std::same_as<std::remove_cvref_t<Type>, TaskResourceAccess> ||
            std::same_as<std::remove_cvref_t<Type>, TaskResourceList> ||
            std::same_as<std::remove_cvref_t<Type>, TaskAffinityProperty> ||
            std::same_as<std::remove_cvref_t<Type>, TaskLifetimePin>;
    }

    class TaskGraph;
    class TaskRunState;
    class TaskWork;

    template <class Executor>
    concept TaskExecutor = requires(Executor& executor, TaskWork&& work)
    {
        { executor.submit(std::move(work)) } noexcept -> std::same_as<void>;
    };

    class LUX_CORE_TASK_PUBLIC TaskGraphBuilder final
    {
      public:
        TaskGraphBuilder() = default;
        ~TaskGraphBuilder() = default;
        TaskGraphBuilder(TaskGraphBuilder&&) noexcept = default;
        TaskGraphBuilder& operator=(TaskGraphBuilder&&) noexcept = default;
        TaskGraphBuilder(const TaskGraphBuilder&) = delete;
        TaskGraphBuilder& operator=(const TaskGraphBuilder&) = delete;

        template <class... Args>
        [[nodiscard]] lux::cxx::expected<TaskId, TaskGraphFailure> add(
            Args&&... args
        ) noexcept
        {
            static_assert(sizeof...(Args) != 0U);
            auto arguments = std::forward_as_tuple(
                std::forward<Args>(args)...
            );
            return addTuple(
                std::move(arguments),
                std::make_index_sequence<sizeof...(Args) - 1U>{}
            );
        }

        [[nodiscard]] lux::cxx::expected<void, TaskGraphFailure> before(
            TaskId before,
            TaskId after
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, TaskGraphFailure> after(
            TaskId after,
            TaskId before
        ) noexcept
        {
            return this->before(before, after);
        }

        [[nodiscard]] std::size_t taskCount() const noexcept
        {
            return tasks_.size();
        }

      private:
        template <class Tuple, std::size_t... Index>
        [[nodiscard]] lux::cxx::expected<TaskId, TaskGraphFailure> addTuple(
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
                detail::TaskDefinition definition;
                collectProperties(
                    definition,
                    std::get<Index>(arguments)...
                );
                auto* callable = new Callable(
                    std::forward<decltype(std::get<kCallableIndex>(arguments))>(
                        std::get<kCallableIndex>(arguments)
                    )
                );
                definition.callable.object = callable;
                definition.callable.invoke = [](const void* value) noexcept
                {
                    std::invoke(*static_cast<const Callable*>(value));
                };
                definition.callable.destroy = [](void* value) noexcept
                {
                    delete static_cast<Callable*>(value);
                };
                return addDefinition(std::move(definition));
            }
            catch (...)
            {
                return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                    .code = ETaskGraphError::ALLOCATION_FAILURE
                });
            }
        }

        static void collectProperty(
            detail::TaskDefinition& definition,
            TaskResourceAccess property
        );
        static void collectProperty(
            detail::TaskDefinition& definition,
            TaskResourceList property
        );
        static void collectProperty(
            detail::TaskDefinition& definition,
            TaskAffinityProperty property
        ) noexcept;
        static void collectProperty(
            detail::TaskDefinition& definition,
            TaskLifetimePin property
        );

        template <class... Properties>
        static void collectProperties(
            detail::TaskDefinition& definition,
            Properties&&... properties
        )
        {
            (collectProperty(
                definition,
                std::forward<Properties>(properties)
            ), ...);
        }

        [[nodiscard]] lux::cxx::expected<TaskId, TaskGraphFailure>
        addDefinition(detail::TaskDefinition definition) noexcept;

        std::vector<detail::TaskDefinition> tasks_;
        std::vector<detail::Edge> explicit_edges_;

        friend LUX_CORE_TASK_PUBLIC lux::cxx::expected<
            TaskGraph,
            TaskGraphFailure
        > compile(TaskGraphBuilder&&) noexcept;
    };

    class LUX_CORE_TASK_PUBLIC TaskGraph final
    {
      public:
        TaskGraph() = default;
        ~TaskGraph() = default;
        TaskGraph(TaskGraph&&) noexcept = default;
        TaskGraph& operator=(TaskGraph&&) noexcept = default;
        TaskGraph(const TaskGraph&) = delete;
        TaskGraph& operator=(const TaskGraph&) = delete;

        [[nodiscard]] std::size_t taskCount() const noexcept
        {
            return tasks_.size();
        }

        [[nodiscard]] std::size_t dependencyCount() const noexcept
        {
            return edges_.size();
        }

        [[nodiscard]] std::size_t lifetimePinCount() const noexcept
        {
            return lifetime_pins_.size();
        }

      private:
        // Destruction is reverse declaration order: callables die before pins.
        std::vector<std::shared_ptr<const void>> lifetime_pins_;
        std::vector<detail::TaskDefinition> tasks_;
        std::vector<std::uint32_t> indegrees_;
        std::vector<std::uint32_t> edge_offsets_;
        std::vector<std::uint32_t> edges_;

        friend LUX_CORE_TASK_PUBLIC lux::cxx::expected<
            TaskGraph,
            TaskGraphFailure
        > compile(TaskGraphBuilder&&) noexcept;
        friend LUX_CORE_TASK_PUBLIC lux::cxx::expected<
            void,
            TaskRunFailure
        > prepare(TaskRunState&, const TaskGraph&) noexcept;
        friend class TaskWork;
        template <TaskExecutor Executor>
        friend lux::cxx::expected<void, TaskRunFailure> run(
            const TaskGraph&,
            Executor&,
            TaskRunState&
        ) noexcept;
    };

    [[nodiscard]] LUX_CORE_TASK_PUBLIC lux::cxx::expected<
        TaskGraph,
        TaskGraphFailure
    > compile(TaskGraphBuilder&& builder) noexcept;

    class LUX_CORE_TASK_PUBLIC TaskRunState final
    {
      public:
        TaskRunState() = default;
        ~TaskRunState() = default;
        TaskRunState(const TaskRunState&) = delete;
        TaskRunState& operator=(const TaskRunState&) = delete;
        TaskRunState(TaskRunState&&) = delete;
        TaskRunState& operator=(TaskRunState&&) = delete;

        [[nodiscard]] std::size_t taskCapacity() const noexcept
        {
            return indegrees_.capacity();
        }

      private:
        std::vector<std::uint32_t> indegrees_;
        std::vector<std::uint8_t> task_states_;
        std::vector<std::uint32_t> ready_;
        std::vector<std::uint32_t> completions_;
        std::size_t completion_head_{};
        std::size_t completion_tail_{};
        std::size_t completion_count_{};
        std::size_t completed_count_{};
        const TaskGraph* prepared_graph_{};
        std::thread::id caller_thread_{};
        std::mutex completion_mutex_;
        std::condition_variable completion_ready_;
        bool running_{};

        friend class TaskWork;
        friend LUX_CORE_TASK_PUBLIC lux::cxx::expected<
            void,
            TaskRunFailure
        > prepare(TaskRunState&, const TaskGraph&) noexcept;
        template <TaskExecutor Executor>
        friend lux::cxx::expected<void, TaskRunFailure> run(
            const TaskGraph&,
            Executor&,
            TaskRunState&
        ) noexcept;
    };

    [[nodiscard]] LUX_CORE_TASK_PUBLIC lux::cxx::expected<
        void,
        TaskRunFailure
    > prepare(TaskRunState& state, const TaskGraph& graph) noexcept;

    class LUX_CORE_TASK_PUBLIC TaskWork final
    {
      public:
        ~TaskWork() noexcept;
        TaskWork(TaskWork&& other) noexcept;
        TaskWork& operator=(TaskWork&&) noexcept = delete;
        TaskWork(const TaskWork&) = delete;
        TaskWork& operator=(const TaskWork&) = delete;

        [[nodiscard]] ETaskAffinity affinity() const noexcept;
        void run() && noexcept;

      private:
        TaskWork(
            const TaskGraph& graph,
            TaskRunState& state,
            std::uint32_t task
        ) noexcept;

        const TaskGraph* graph_{};
        TaskRunState* state_{};
        std::uint32_t task_{};
        bool active_{};

        template <TaskExecutor Executor>
        friend lux::cxx::expected<void, TaskRunFailure> run(
            const TaskGraph&,
            Executor&,
            TaskRunState&
        ) noexcept;
    };

    class InlineTaskExecutor final
    {
      public:
        void submit(TaskWork&& work) noexcept
        {
            std::move(work).run();
        }
    };

    template <TaskExecutor Executor>
    [[nodiscard]] lux::cxx::expected<void, TaskRunFailure> run(
        const TaskGraph& graph,
        Executor& executor,
        TaskRunState& state
    ) noexcept
    {
        if (state.prepared_graph_ != std::addressof(graph))
        {
            return lux::cxx::unexpected<TaskRunFailure>(TaskRunFailure{
                ETaskRunError::NOT_PREPARED
            });
        }
        if (state.running_)
        {
            return lux::cxx::unexpected<TaskRunFailure>(TaskRunFailure{
                ETaskRunError::ALREADY_RUNNING
            });
        }

        state.running_ = true;
        state.caller_thread_ = std::this_thread::get_id();
        state.completed_count_ = 0U;
        state.completion_head_ = 0U;
        state.completion_tail_ = 0U;
        state.completion_count_ = 0U;
        std::copy(
            graph.indegrees_.begin(),
            graph.indegrees_.end(),
            state.indegrees_.begin()
        );
        std::fill(
            state.task_states_.begin(),
            state.task_states_.end(),
            std::uint8_t{0}
        );
        state.ready_.clear();
        const auto ready_order = std::greater<std::uint32_t>{};
        for (std::uint32_t task{}; task < graph.taskCount(); ++task)
        {
            if (state.indegrees_[task] == 0U)
            {
                state.ready_.push_back(task);
                std::push_heap(
                    state.ready_.begin(),
                    state.ready_.end(),
                    ready_order
                );
            }
        }

        while (state.completed_count_ != graph.taskCount())
        {
            while (!state.ready_.empty())
            {
                std::pop_heap(
                    state.ready_.begin(),
                    state.ready_.end(),
                    ready_order
                );
                const std::uint32_t task = state.ready_.back();
                state.ready_.pop_back();
                if (state.task_states_[task] != 0U)
                    detail::taskContractFailure();
                state.task_states_[task] = 1U;
                TaskWork work(graph, state, task);
                if (work.affinity() == ETaskAffinity::CALLER_THREAD)
                    std::move(work).run();
                else
                    executor.submit(std::move(work));
            }

            std::unique_lock lock(state.completion_mutex_);
            state.completion_ready_.wait(lock, [&state]
            {
                return state.completion_count_ != 0U;
            });
            const std::uint32_t completed =
                state.completions_[state.completion_head_];
            state.completion_head_ =
                (state.completion_head_ + 1U) % state.completions_.size();
            --state.completion_count_;
            lock.unlock();

            state.task_states_[completed] = 2U;
            ++state.completed_count_;
            for (std::uint32_t edge = graph.edge_offsets_[completed];
                 edge < graph.edge_offsets_[completed + 1U]; ++edge)
            {
                const std::uint32_t dependent = graph.edges_[edge];
                if (state.indegrees_[dependent] == 0U)
                    detail::taskContractFailure();
                --state.indegrees_[dependent];
                if (state.indegrees_[dependent] == 0U)
                {
                    state.ready_.push_back(dependent);
                    std::push_heap(
                        state.ready_.begin(),
                        state.ready_.end(),
                        ready_order
                    );
                }
            }
        }
        state.running_ = false;
        return {};
    }
}
