#pragma once

#include <lux/engine/task/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/container/SlotMap.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::task
{
    struct TaskTag;
    using TaskId = lux::cxx::SlotKey<
        TaskTag,
        std::uint32_t,
        std::uint32_t
    >;

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

    struct TaskExecutionItem final
    {
        TaskId          id{};
        ETaskAffinity   affinity{ETaskAffinity::WORKER};
        TaskInvocation  invocation{};
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
        TaskId          task{};
        TaskId          related{};
    };

    class TaskGraphBuilder;
    class TaskGraph;
    class TaskExecutionScratch;
    struct TaskExecutionBackendRef;

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
        pinCode(std::shared_ptr<const void> lifetime) noexcept;

        [[nodiscard]] lux::cxx::expected<TaskGraph, TaskGraphFailure>
        build() && noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    struct TaskExecutionBackendRef final
    {
        void* state{};
        void (*execute_wave)(
            void*,
            std::span<const TaskExecutionItem>,
            void*
        ) noexcept{};
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
