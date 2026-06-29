#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Thin, engine-idiomatic facade over the vendored nativefiledialog-extended
// (engine/thirdparty/nativefiledialog). It exposes ONLY std types so the rest
// of the editor never sees nfd's C API — the nfd headers are an implementation
// detail of FileDialog.cpp.
//
// THREADING: every function shows a BLOCKING, owner-modal OS dialog and runs the
// platform's modal message loop until the user picks or cancels. It MUST be
// called from the thread that owns the window and pumps its message loop. In the
// editor that is the main thread, specifically from the `pending_actions_` drain
// (between frames, outside any open ImGui frame) — never from inside a panel
// paint. See LuxEditor::run().

namespace lux::editor
{
    /// One entry in a native file-dialog filter list. `spec` is a comma-separated
    /// list of extensions WITHOUT the leading dot, exactly as
    /// nativefiledialog-extended expects, e.g. name="3D model",
    /// spec="gltf,glb,fbx,obj". An empty filter list shows all files.
    struct FileFilter
    {
        std::string name;
        std::string spec;
    };

    /// Native OS "open file" dialog, owner-modal to `parent_native_window`.
    /// Returns the chosen path, or std::nullopt on cancel / error.
    /// `parent_native_window` is the platform window handle (HWND on Windows,
    /// from LuxWindow::win32Handle()); pass nullptr for an unparented dialog.
    std::optional<std::filesystem::path> openFileDialog(
        void*                          parent_native_window,
        const std::vector<FileFilter>& filters      = {},
        const std::filesystem::path&   default_path = {});

    /// Native OS "save file" dialog. `default_name` pre-fills the filename box.
    /// Returns the chosen path (the user may type a not-yet-existing file), or
    /// std::nullopt on cancel / error.
    std::optional<std::filesystem::path> saveFileDialog(
        void*                          parent_native_window,
        const std::vector<FileFilter>& filters      = {},
        const std::filesystem::path&   default_path = {},
        const std::string&             default_name = {});

    /// Native OS "pick folder" dialog. Returns the chosen directory, or
    /// std::nullopt on cancel / error.
    std::optional<std::filesystem::path> pickFolderDialog(
        void*                        parent_native_window,
        const std::filesystem::path& default_path = {});
}
