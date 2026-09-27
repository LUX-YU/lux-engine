#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/process/Task.hpp>

namespace lux::editor::detail
{
    template <class T> [[nodiscard]] EditorResult<T> taskResult(process::TTaskResult<T, EditorFailure> result) noexcept
    {
        if (result)
        {
            if constexpr (std::is_void_v<T>)
                return {};
            else
                return std::move(*result);
        }
        auto& error = result.error();
        if (auto* failure = error.domainFailure())
            return lux::cxx::unexpected(std::move(*failure));
        if (const auto* failure = error.executionFailure())
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::EXECUTION_FAILURE,
                "process.task",
                static_cast<std::uint64_t>(*failure),
                {},
                *failure
            });
        return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "process.task"});
    }
}
