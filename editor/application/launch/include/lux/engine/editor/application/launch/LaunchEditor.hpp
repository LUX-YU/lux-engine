#pragma once
#include <lux/engine/editor/EditorError.hpp>
#include <filesystem>

namespace lux::editor
{
    // Blocking OS launch. The caller schedules it through ExecutionRuntime.
    [[nodiscard]] EditorResult<void> launchEditor(
        const std::filesystem::path& installation,
        const std::filesystem::path& project_file
    ) noexcept;
}
