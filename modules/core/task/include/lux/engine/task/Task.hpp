#pragma once

#include <lux/engine/task/visibility.h>

#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace lux::task
{
    inline constexpr std::uint32_t InvalidTaskIndex = 0xffff'ffffU;

    struct TaskBuildId final
    {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool isValid() const noexcept
        {
            return value != 0U;
        }

        [[nodiscard]] constexpr bool operator==(const TaskBuildId&) const noexcept = default;
    };

    /**
     * Builder-local task handle. It is intentionally not a runtime TaskGraph ID.
     * A handle may only be used by the TaskGraphBuilder that produced it.
     */
    struct TaskHandle final
    {
        TaskBuildId owner{};
        std::uint32_t index{InvalidTaskIndex};

        [[nodiscard]] constexpr bool isValid() const noexcept
        {
            return owner.isValid() && index != InvalidTaskIndex;
        }

        [[nodiscard]] constexpr bool operator==(const TaskHandle&) const noexcept = default;
    };

    struct TaskResourceKey final
    {
        std::uint64_t domain{};
        std::uint64_t value{};

        [[nodiscard]] constexpr bool operator==(const TaskResourceKey&) const noexcept = default;

        [[nodiscard]] constexpr auto operator<=>(const TaskResourceKey&) const noexcept = default;
    };

    enum class ETaskResourceAccess : std::uint8_t
    {
        READ,
        WRITE,
    };

    enum class ETaskAffinity : std::uint8_t
    {
        /** Executed by a background worker when workers are configured. */
        WORKER,

        /** Executed only by the thread that calls TaskExecutor::execute(). */
        CALLER_THREAD,
    };

    struct TaskResourceAccess final
    {
        TaskResourceKey key{};
        ETaskResourceAccess access{ETaskResourceAccess::READ};
    };

    /** Owning convenience property. Consumed immediately by TaskGraphBuilder::add. */
    struct TaskResources final
    {
        std::vector<TaskResourceAccess> values;
    };

    struct TaskDependency final
    {
        TaskHandle task{};
    };

    /** Owning dynamic dependency property. Consumed immediately by TaskGraphBuilder::add. */
    struct TaskDependencies final
    {
        std::vector<TaskHandle> values;
    };

    struct TaskAffinity final
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

    [[nodiscard]] inline TaskResources resources(std::span<const TaskResourceAccess> accesses)
    {
        return TaskResources{std::vector<TaskResourceAccess>(accesses.begin(), accesses.end())};
    }

    template <class Range>
        requires requires(const Range& value) { std::span<const TaskResourceAccess>(value); }
    [[nodiscard]] TaskResources resources(const Range& accesses)
    {
        return resources(std::span<const TaskResourceAccess>(accesses));
    }

    [[nodiscard]] constexpr TaskDependency dependsOn(TaskHandle task) noexcept
    {
        return {task};
    }

    [[nodiscard]] inline TaskDependencies dependencies(std::span<const TaskHandle> values)
    {
        return TaskDependencies{std::vector<TaskHandle>(values.begin(), values.end())};
    }

    template <class Range>
        requires requires(const Range& value) { std::span<const TaskHandle>(value); }
    [[nodiscard]] TaskDependencies dependencies(const Range& values)
    {
        return dependencies(std::span<const TaskHandle>(values));
    }

    [[nodiscard]] constexpr TaskAffinity on(ETaskAffinity affinity) noexcept
    {
        return {affinity};
    }

    [[nodiscard]] inline TaskLifetimePin keepAlive(std::shared_ptr<const void> lifetime) noexcept
    {
        return {std::move(lifetime)};
    }

    enum class ETaskGraphError : std::uint8_t
    {
        INVALID_BUILDER,
        INVALID_TASK,
        INVALID_CALLABLE,
        INVALID_RESOURCE,
        DUPLICATE_RESOURCE,
        DUPLICATE_DEPENDENCY,
        DEPENDENCY_MUST_PRECEDE_TASK,
        ALLOCATION_FAILURE,
    };

    struct TaskGraphFailure final
    {
        ETaskGraphError code{ETaskGraphError::INVALID_TASK};
        TaskHandle task{};
        TaskHandle related{};
        TaskResourceKey resource{};
    };
}
