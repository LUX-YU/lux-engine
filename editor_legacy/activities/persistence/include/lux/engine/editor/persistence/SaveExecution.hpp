#pragma once

#include <lux/engine/editor/persistence/SaveService.hpp>
#include <lux/engine/process/TaskScope.hpp>

namespace lux::editor::persistence
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
        // A service allocation retains its exact providers until TaskScope has drained. Non-null inputs.
        SaveExecution(
            process::ExecutionRuntime& runtime,
            std::shared_ptr<SaveService> service,
            std::shared_ptr<WriteCoordinator> coordinator,
            std::shared_ptr<IArtifactStore> store
        );
        ~SaveExecution();
        SaveExecution(const SaveExecution&) = delete;
        SaveExecution& operator=(const SaveExecution&) = delete;
        SaveExecution(SaveExecution&&) = delete;
        SaveExecution& operator=(SaveExecution&&) = delete;
        [[nodiscard]] persistence::PersistenceResult<void> submitReady();
        [[nodiscard]] process::TaskScope& tasks() noexcept
        {
            return tasks_;
        }

    private:
        std::shared_ptr<SaveService> service_owner_;
        std::shared_ptr<WriteCoordinator> coordinator_owner_;
        std::shared_ptr<IArtifactStore> store_owner_;
        process::TaskScope tasks_;
        persistence::SaveService& service_;
        persistence::WriteCoordinator& coordinator_;
        persistence::IArtifactStore& store_;
    };
}
