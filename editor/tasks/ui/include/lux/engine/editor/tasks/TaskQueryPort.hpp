#pragma once
#include <lux/engine/process/ExecutionRuntime.hpp>
namespace lux::editor::tasks
{
    // Borrowed owner-thread queries and cancellation only. It cannot submit, dispatch or own work.
    class TaskQueryPort final
    {
    public:
        explicit TaskQueryPort(process::ExecutionRuntime& runtime) noexcept : runtime_(runtime) {}
        using Revision = std::uint64_t (*)(const void*) noexcept;
        TaskQueryPort(process::ExecutionRuntime& runtime, const void* owner, Revision revision) noexcept
            : runtime_(runtime), owner_(owner), revision_(revision)
        {}
        [[nodiscard]] std::vector<process::TaskInfo> query() const
        {
            return runtime_.taskInfos();
        }
        [[nodiscard]] bool cancel(process::TaskId id) const noexcept
        {
            return runtime_.requestStop(id);
        }
        [[nodiscard]] std::optional<std::uint64_t> revision() const noexcept
        {
            return revision_ ? std::optional{revision_(owner_)} : std::nullopt;
        }

    private:
        process::ExecutionRuntime& runtime_;
        const void* owner_{};
        Revision revision_{};
    };
}
