#pragma once
/**
 * @file SpawnHelpers.hpp  (header-only)
 * @brief Cross-cutting "spawn a sibling exe" helpers used by both
 *        `lux_editor` and `lux_launcher`.
 *
 *  - `lux_launcher` calls `spawnEditorForProject(...)` after the user
 *    picks a project, then exits.
 *  - `lux_editor` calls `spawnLauncher()` from its no-argv main entry
 *    (double-click case) and from File→Switch Project.
 *
 * Windows-only for now: uses `CreateProcessW` + `GetModuleFileNameW`
 * to find the calling exe's directory and spawn a sibling there
 * (avoids `PATH` assumptions). Cross-platform abstraction is out of
 * scope for M3.5 — when needed, gate on `#ifdef _WIN32`.
 *
 * Header-only on purpose: this is the only piece that `lux_editor` needs
 * from `engine/hosts/launcher/`. Pulling in a static library just for two
 * functions would require linking `imgui` and Vulkan transitively into
 * the editor — which it already has, but the build-graph dependency
 * direction (`editor → launcher`) reads wrong. Inline header keeps the
 * dependency clean.
 */

#include <filesystem>
#include <string>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    // Suppress the min/max macros — they clash with std::numeric_limits<>::max()
    // and other `max()` calls in template headers the editor / launcher
    // transitively include.
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace lux::launcher
{
    namespace detail
    {
        /// Absolute path of the directory that contains the currently-
        /// running executable. Used to anchor sibling-exe lookups so we
        /// don't rely on `PATH`.
        inline std::filesystem::path currentExecutableDir()
        {
#ifdef _WIN32
            wchar_t buf[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
            if (n == 0 || n == MAX_PATH)
                return std::filesystem::current_path();
            return std::filesystem::path(buf).parent_path();
#else
            return std::filesystem::current_path();
#endif
        }
    } // namespace detail

#ifdef _WIN32
    /// Start a sibling executable next to the calling exe, do not wait
    /// for it. Returns true on successful `CreateProcessW` (the new
    /// process may still crash immediately; we only check that the
    /// system accepted the spawn request). The caller typically
    /// `exit(0)`s right after.
    ///
    /// `extra_args` is appended after the exe path on the command line;
    /// supply quoted paths if they may contain spaces.
    inline bool spawnSibling(const wchar_t*      exe_name,
                             const std::wstring& extra_args = {})
    {
        const auto exe_path = detail::currentExecutableDir() / exe_name;
        if (!std::filesystem::exists(exe_path))
            return false;

        // CreateProcessW's lpCommandLine is mutable in/out — copy into
        // a local buffer it can scribble on.
        std::wstring cmd_line = L"\"" + exe_path.wstring() + L"\"";
        if (!extra_args.empty())
        {
            cmd_line += L" ";
            cmd_line += extra_args;
        }

        STARTUPINFOW        si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        // CREATE_NEW_CONSOLE: the spawned process gets its own console
        // window. Without this the child inherits the parent's, which is
        // useless once the parent (e.g. the launcher) exits — any stderr
        // the child wrote after the parent's console closed disappears,
        // and on a crash the user only sees a Windows fault dialog with
        // no stack info. With a separate console, the child's stderr
        // survives until the child itself exits.
        const BOOL ok = CreateProcessW(
            /*lpApplicationName*/   exe_path.c_str(),
            /*lpCommandLine*/       cmd_line.data(),
            /*lpProcessAttributes*/ nullptr,
            /*lpThreadAttributes*/  nullptr,
            /*bInheritHandles*/     FALSE,
            /*dwCreationFlags*/     CREATE_NEW_CONSOLE,
            /*lpEnvironment*/       nullptr,
            /*lpCurrentDirectory*/  nullptr,
            /*lpStartupInfo*/       &si,
            /*lpProcessInformation*/&pi);

        if (!ok)
            return false;

        // Close handles immediately — we don't wait. The child runs
        // independently; the parent typically exits next.
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
#else
    inline bool spawnSibling(const wchar_t* /*exe_name*/,
                             const std::wstring& /*extra_args*/ = {})
    {
        // POSIX fallback intentionally not implemented in M3.5.
        return false;
    }
#endif

    /// Launch `lux_editor.exe` with `--project "<manifest>"`. The caller
    /// (typically `lux_launcher`) should `exit(0)` right after, letting
    /// the editor own the user session.
    inline bool spawnEditorForProject(const std::filesystem::path& manifest)
    {
        // Double-quote the path so spaces in the project root don't
        // split the editor's argv.
        const std::wstring args = L"--project \"" + manifest.wstring() + L"\"";
        return spawnSibling(L"lux_editor.exe", args);
    }

    /// Launch `lux_launcher.exe` with no arguments. The caller
    /// (typically `lux_editor` from File→Switch Project, or from main
    /// when no `--project` was supplied) should `exit(0)` or
    /// `requestQuit()` right after.
    inline bool spawnLauncher()
    {
        return spawnSibling(L"lux_launcher.exe");
    }

} // namespace lux::launcher
