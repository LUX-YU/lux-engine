#include <lux/engine/editor/material/MaterialCompilationService.hpp>
#include <algorithm>

namespace lux::editor::material
{
    namespace
    {
        auto rejected(EMaterialCompileRequestError error)
        {
            return cxx::unexpected(VMaterialCompileFailure{error});
        }
    }
    MaterialCompilationService::MaterialCompilationService(process::ExecutionRuntime& runtime, std::size_t capacity)
        : runtime_(runtime), capacity_(capacity)
    {
        operations_.reserve(capacity);
    }
    MaterialCompilationService::~MaterialCompilationService() = default;
    MaterialCompileResult<std::vector<MaterialCompileId>> MaterialCompilationService::snapshotIds() const
    {
        if (owner_ != std::this_thread::get_id())
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        std::vector<MaterialCompileId> result;
        result.reserve(operations_.size());
        for (const auto& operation : operations_)
            result.push_back(operation.operation->id());
        return result;
    }
    MaterialCompileResult<MaterialCompileId> MaterialCompilationService::start(
        MaterialSnapshot snapshot,
        MaterialCompileSettings settings,
        std::uint64_t environment
    )
    {
        if (owner_ != std::this_thread::get_id())
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        if (operations_.size() == capacity_)
            return rejected(EMaterialCompileRequestError::CAPACITY);
        auto operation = MaterialCompileOperation::start(runtime_, std::move(snapshot), settings, environment);
        if (!operation)
            return cxx::unexpected(operation.error());
        const auto id = (*operation)->id();
        operations_.push_back({std::move(*operation)});
        return id;
    }
    MaterialCompileResult<std::reference_wrapper<const MaterialCompileOperation>> MaterialCompilationService::operation(
        MaterialCompileId id
    ) const noexcept
    {
        if (owner_ != std::this_thread::get_id())
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        const auto found = std::ranges::find_if(operations_, [id](const auto& value) { return value.operation->id() == id; });
        if (found == operations_.end())
            return rejected(EMaterialCompileRequestError::INVALID_ID);
        return std::cref(*found->operation);
    }
    MaterialCompileResult<void> MaterialCompilationService::acknowledge(MaterialCompileId id)
    {
        if (owner_ != std::this_thread::get_id())
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        const auto found = std::ranges::find_if(operations_, [id](const auto& value) { return value.operation->id() == id; });
        if (found == operations_.end())
            return rejected(EMaterialCompileRequestError::INVALID_ID);
        if (!found->operation->ready())
            return rejected(EMaterialCompileRequestError::BUSY);
        auto retiring = std::move(found->operation);
        operations_.erase(found); // Erase before callbacks can reenter through payload/code destruction.
        return {};
    }
    MaterialCompileResult<void> MaterialCompilationService::cancel(MaterialCompileId id) noexcept
    {
        if (owner_ != std::this_thread::get_id())
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        const auto found = std::ranges::find_if(operations_, [id](const auto& value) { return value.operation->id() == id; });
        if (found == operations_.end())
            return rejected(EMaterialCompileRequestError::INVALID_ID);
        found->operation->cancel();
        return {};
    }    MaterialCompileResult<void> MaterialCompilationService::releaseResult(MaterialCompileId id) noexcept
    {
        if (owner_ != std::this_thread::get_id())
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        const auto found = std::ranges::find_if(operations_, [id](const auto& value) {
            return value.operation->id() == id;
        });
        if (found != operations_.end())
            found->released = true;
        return {};
    }
    MaterialCompileResult<void> MaterialCompilationService::collectReleased()
    {
        if (owner_ != std::this_thread::get_id())
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        std::vector<MaterialCompileId> ready;
        for (const auto& record : operations_)
            if (record.released && record.operation->ready())
                ready.push_back(record.operation->id());
        for (auto id : ready)
        {
            auto acknowledged = acknowledge(id);
            if (!acknowledged)
            {
                const auto* error = std::get_if<EMaterialCompileRequestError>(&acknowledged.error());
                if (!error || *error != EMaterialCompileRequestError::INVALID_ID)
                    return acknowledged;
            }
        }
        return {};
    }
    bool MaterialCompilationService::empty() const noexcept
    {
        return operations_.empty();
    }

}
