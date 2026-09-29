#pragma once

#include <lux/engine/editor/persistence/SaveService.hpp>
#include <lux/engine/process/TaskScope.hpp>

namespace lux::editor::io
{
    // Narrow scheduler binding. Destroy before service/coordinator/store; TaskScope drains accepted work.
    class SaveExecution final
    {
    public:
        SaveExecution(
            process::ExecutionRuntime& runtime,
            persistence::SaveService& service,
            persistence::WriteCoordinator& coordinator,
            persistence::IArtifactStore& store
        );
        ~SaveExecution();
        SaveExecution(const SaveExecution&) = delete;
        SaveExecution& operator=(const SaveExecution&) = delete;
        [[nodiscard]] persistence::PersistenceResult<void> submitReady();
        [[nodiscard]] process::TaskScope& tasks() noexcept
        {
            return tasks_;
        }

    private:
        process::TaskScope tasks_;
        persistence::SaveService& service_;
        persistence::WriteCoordinator& coordinator_;
        persistence::IArtifactStore& store_;
    };
}
