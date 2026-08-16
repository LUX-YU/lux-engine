#pragma once
/**
 * @file RecentProjects.hpp
 * @brief Cross-process recent-projects list at `%APPDATA%/Lux/recent.txt`.
 *
 * One absolute manifest path per line, newest at the top, capped at
 * 10 entries. Both `lux_editor.exe` and `lux_launcher.exe` read and
 * write the same file:
 *
 *   - `lux_launcher`:
 *       * `loadRecentProjects()` at startup to populate the picker.
 *       * `pushRecentProject(path)` after the user selects an existing
 *         project (move-to-front so the next launcher boot ranks it
 *         first).
 *
 *   - `lux_editor`:
 *       * `pushRecentProject(path)` after a successful `openProject` so
 *         the launcher and editor's File→Recent submenu both pick it
 *         up next time.
 *       * `loadRecentProjects()` to populate the editor's File→Recent
 *         submenu while the editor is open.
 *
 * The file is plain text, one path per line. No locking — collisions
 * are rare (editor and launcher don't run simultaneously in the normal
 * flow), and the worst case is one launcher session missing a recent
 * push the editor wrote concurrently. Acceptable for M3.5.
 *
 * Lives in Authoring so the editor and launcher share the same workspace
 * history without exposing it to Player.
 */

#include <lux/engine/authoring/project/visibility.h>

#include <filesystem>
#include <vector>

namespace lux::authoring
{
    /// Maximum number of entries kept in the recent-projects file.
    /// `pushRecentProject` truncates anything beyond this cap.
    inline constexpr std::size_t kRecentProjectsCap = 10u;

    /// Absolute path of the recent-projects file.
    ///   Windows : `%APPDATA%/Lux/recent.txt`
    ///   POSIX   : `$HOME/.config/lux/recent.txt`
    ///   fallback: `<cwd>/.lux_recent.txt`
    [[nodiscard]] LUX_ENGINE_AUTHORING_PROJECT_PUBLIC
        std::filesystem::path recentProjectsPath();

    /// Read the file, returning one entry per non-empty line. Missing
    /// file produces an empty vector (not an error).
    [[nodiscard]] LUX_ENGINE_AUTHORING_PROJECT_PUBLIC
        std::vector<std::filesystem::path> loadRecentProjects();

    /// Overwrite the file with `list` (one entry per line, in order).
    /// Creates the parent directory if needed. Best-effort: silent on
    /// I/O errors (caller has no recovery story in M3.5).
    LUX_ENGINE_AUTHORING_PROJECT_PUBLIC
        void saveRecentProjects(const std::vector<std::filesystem::path>& list);

    /// Move-to-front + dedupe + truncate at `kRecentProjectsCap`. The
    /// path is recorded as `std::filesystem::absolute(p)` so launcher
    /// and editor agree on a canonical representation regardless of
    /// which working directory each was launched from.
    LUX_ENGINE_AUTHORING_PROJECT_PUBLIC
        void pushRecentProject(const std::filesystem::path& p);

} // namespace lux::authoring
