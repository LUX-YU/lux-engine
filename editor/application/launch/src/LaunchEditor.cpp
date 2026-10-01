#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/platform/Process.hpp>
#include <array>

namespace lux::editor
{
    EditorResult<void> launchEditor(
        const std::filesystem::path& installation,
        const std::filesystem::path& project_file
    ) noexcept
    {
        const auto encoded = project_file.u8string();
        const std::array arguments{std::string{"--project"}, std::string(encoded.begin(), encoded.end())};
        auto executable = engine::platform::executablePath();
        if (!executable)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "editor.executable", executable.error().native_code}
            );
        auto editor = installation / "bin/lux_editor";
        editor += executable->extension();
        const auto launched = engine::platform::launchProcess(editor, arguments);
        if (!launched)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "editor.launch",
                launched.error().native_code,
                "The project remains saved. Check the Editor installation and retry opening it."
            });
        return {};
    }
}
