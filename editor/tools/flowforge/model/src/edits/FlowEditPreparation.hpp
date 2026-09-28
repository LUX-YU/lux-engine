#pragma once
#include <lux/engine/editor/flowforge/FlowEdit.hpp>

namespace lux::editor::flowforge::detail
{
    inline auto rejected(EFlowEditError code)
    {
        return lux::cxx::unexpected(FlowEditError{code});
    }
    inline FlowEditError historyFailure(editing::EditFailure failure) noexcept
    {
        FlowEditError result{EFlowEditError::HISTORY};
        result.history = failure;
        return result;
    }
    inline FlowEditError sourceFailure(lux::flowforge::FlowSourceFailure failure)
    {
        FlowEditError result{EFlowEditError::INVALID_SOURCE};
        result.source = std::move(failure);
        return result;
    }
    [[nodiscard]] std::size_t sourceBytes(const FlowAuthoringSource& source) noexcept;
}
