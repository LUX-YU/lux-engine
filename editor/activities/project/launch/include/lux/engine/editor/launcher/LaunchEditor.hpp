#pragma once
#include <filesystem>
#include <lux/engine/editor/EditorError.hpp>

namespace lux::editor
{
    // Blocking OS launch. The caller schedules it through ExecutionRuntime.
    [[nodiscard]] EditorResult<void> launchEditor(
        const std::filesystem::path& installation,
        const std::filesystem::path& project_file
    ) noexcept;
} // namespace lux::editor
