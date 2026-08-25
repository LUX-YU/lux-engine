#pragma once

#include <lux/engine/task/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/container/ScopeId.hpp>
#include <lux/cxx/container/SlotMap.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::task
{
    struct TaskGraphScopeTag;
    using TaskGraphId = lux::cxx::ScopeId<TaskGraphScopeTag>;

    struct TaskSlotTag;
    using TaskSlot = lux::cxx::SlotKey<
        TaskSlotTag,
        std::uint32_t,
        std::uint32_t
    >;

    struct TaskId final
    {
        TaskGraphId owner{};
        TaskSlot slot{};

        [[nodiscard]] constexpr bool isNull() const noexcept
        {
            return owner.isNull() || slot.isNull();
        }

        [[nodiscard]] constexpr bool isValid() const noexcept
        {
            return !isNull();
        }

        [[nodiscard]] constexpr bool operator==(
            const TaskId&
        ) const noexcept = default;
    };

    enum class ETaskAffinity : std::uint8_t
    {
        WORKER,
        OWNER_THREAD
    };

    struct TaskInvocation final
    {
        void* target{};
        void (*invoke)(void*, void*) noexcept{};
    };

    enum class ETaskGraphError : std::uint8_t
    {
        INVALID_TASK,
        INVALID_INVOCATION,
        DUPLICATE_DEPENDENCY,
        DEPENDENCY_CYCLE,
        ALLOCATION_FAILURE
    };

    struct TaskGraphFailure final
    {
        ETaskGraphError code{ETaskGraphError::INVALID_TASK};
        TaskId task{};
        TaskId related{};
    };

    class TaskGraph;
    class TaskGraphBuilder;
    class TaskExecutionScratch;
    class TaskSubmission;
    struct TaskExecutionBackendRef;

    class LUX_CORE_TASK_PUBLIC TaskSubmission final
    {
    public:
        ~TaskSubmission() noexcept;

        TaskSubmission(TaskSubmission&& other) noexcept;
        TaskSubmission& operator=(TaskSubmission&& other) noexcept;

        TaskSubmission(const TaskSubmission&) = delete;
        TaskSubmission& operator=(const TaskSubmission&) = delete;

        [[nodiscard]] ETaskAffinity affinity() const noexcept;

        /** Execute the task and report completion exactly once. */
        void run() && noexcept;

    private:
        TaskSubmission(
            ETaskAffinity affinity,
            TaskInvocation invocation,
            void* execution_context,
            void* completion_state,
            std::uint32_t dense_index,
            bool (*on_owner_thread)(void*) noexcept,
            void (*complete)(void*, std::uint32_t) noexcept
        ) noexcept;

        void abandonMovedFrom() noexcept;

        ETaskAffinity affinity_{ETaskAffinity::WORKER};
        TaskInvocation invocation_{};
        void* execution_context_{};
        void* completion_state_{};
        std::uint32_t dense_index_{};
        bool (*on_owner_thread_)(void*) noexcept{};
        void (*complete_)(void*, std::uint32_t) noexcept{};
        bool active_{};

        friend LUX_CORE_TASK_PUBLIC void executeTaskGraph(
            TaskExecutionBackendRef,
            const TaskGraph&,
            void*,
            TaskExecutionScratch&
        ) noexcept;
    };

    struct TaskExecutionBackendRef final
    {
        void* state{};
        void (*submit)(void*, TaskSubmission&&) noexcept{};
    };

    LUX_CORE_TASK_PUBLIC void executeTaskGraph(
        TaskExecutionBackendRef backend,
        const TaskGraph& graph,
        void* execution_context,
        TaskExecutionScratch& scratch
    ) noexcept;

    class LUX_CORE_TASK_PUBLIC TaskGraph final
    {
    public:
        TaskGraph() noexcept;
        ~TaskGraph();

        TaskGraph(TaskGraph&&) noexcept;
        TaskGraph& operator=(TaskGraph&&) noexcept;

        TaskGraph(const TaskGraph&) = delete;
        TaskGraph& operator=(const TaskGraph&) = delete;

        [[nodiscard]] TaskGraphId id() const noexcept;
        [[nodiscard]] std::size_t taskCount() const noexcept;
        [[nodiscard]] std::size_t dependencyCount() const noexcept;
        [[nodiscard]] std::size_t codeLifetimeCount() const noexcept;
        [[nodiscard]] bool contains(TaskId id) const noexcept;

    private:
        struct Impl;
        explicit TaskGraph(std::unique_ptr<const Impl> impl) noexcept;

        std::unique_ptr<const Impl> impl_;

        friend class TaskGraphBuilder;
        friend class TaskExecutionScratch;
        friend LUX_CORE_TASK_PUBLIC void executeTaskGraph(
            TaskExecutionBackendRef,
            const TaskGraph&,
            void*,
            TaskExecutionScratch&
        ) noexcept;
    };

    class LUX_CORE_TASK_PUBLIC TaskGraphBuilder final
    {
    public:
        TaskGraphBuilder();
        ~TaskGraphBuilder();

        TaskGraphBuilder(TaskGraphBuilder&&) noexcept;
        TaskGraphBuilder& operator=(TaskGraphBuilder&&) noexcept;

        TaskGraphBuilder(const TaskGraphBuilder&) = delete;
        TaskGraphBuilder& operator=(const TaskGraphBuilder&) = delete;

        [[nodiscard]] lux::cxx::expected<TaskId, TaskGraphFailure>
        addTask(
            TaskInvocation invocation,
            ETaskAffinity affinity = ETaskAffinity::WORKER
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, TaskGraphFailure>
        addDependency(TaskId before, TaskId after) noexcept;

        [[nodiscard]] lux::cxx::expected<void, TaskGraphFailure>
        pinCodeLifetime(std::shared_ptr<const void> lifetime) noexcept;

        [[nodiscard]] lux::cxx::expected<TaskGraph, TaskGraphFailure>
        build() && noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    class LUX_CORE_TASK_PUBLIC TaskExecutionScratch final
    {
    public:
        TaskExecutionScratch();
        ~TaskExecutionScratch();

        TaskExecutionScratch(TaskExecutionScratch&&) noexcept;
        TaskExecutionScratch& operator=(TaskExecutionScratch&&) noexcept;

        TaskExecutionScratch(const TaskExecutionScratch&) = delete;
        TaskExecutionScratch& operator=(const TaskExecutionScratch&) = delete;

        [[nodiscard]] lux::cxx::expected<void, TaskGraphFailure>
        prepare(const TaskGraph& graph) noexcept;

        [[nodiscard]] std::size_t taskCapacity() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend class TaskSubmission;
        friend LUX_CORE_TASK_PUBLIC void executeTaskGraph(
            TaskExecutionBackendRef,
            const TaskGraph&,
            void*,
            TaskExecutionScratch&
        ) noexcept;
    };

    [[nodiscard]] LUX_CORE_TASK_PUBLIC TaskExecutionBackendRef
    referenceTaskExecutionBackend() noexcept;
}
