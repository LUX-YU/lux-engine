/**
 * @file engine/launcher/app/main.cpp
 * @brief Entry point for the standalone lux-engine project launcher.
 *
 * The launcher's job is small:
 *
 *   1. Open a small window with a Project Picker UI.
 *   2. Wait for the user to pick / open / create a project.
 *   3. Spawn `lux_editor.exe --project <path>` next to the launcher
 *      executable.
 *   4. Exit.
 *
 * If the user closes the launcher window without picking, the
 * launcher just exits with code 1 (cancel). The launcher does NOT
 * try to fall back to a default project — that policy belongs to
 * the editor itself (which currently does have a demo-scene
 * fallback for `--no-launcher` developer use).
 */

#include <lux/engine/launcher/LauncherApp.hpp>

#include <cstdio>
#include <utility>

int main(int /*argc*/, char* /*argv*/[])
{
    lux::launcher::LauncherConfig cfg;
    cfg.title  = "Lux Launcher";
    cfg.width  = 900;
    cfg.height = 600;

    lux::launcher::LauncherApp app(std::move(cfg));
    if (!app.init())
    {
        std::fprintf(stderr, "Failed to initialise launcher\n");
        return 1;
    }
    return app.run();
}
