/**
 * @file engine/editor/app/main.cpp
 * @brief Entry point for the standalone lux-engine editor.
 *
 * Startup policy:
 *
 *   1. `--project <path>` (or `-p`) on argv → init editor + open that
 *      project + run.
 *
 *   2. Otherwise (typical double-click, missing argv) → spawn
 *      `lux_launcher.exe` and exit. The launcher will pick a project
 *      and re-spawn this executable with `--project`.
 *
 * The editor edits exactly one project per process. There is no
 * built-in "demo scene fallback" — demo content belongs to project
 * templates, which is the launcher's responsibility. If a `--project`
 * is supplied but the manifest is broken or the default scene fails
 * to load, the editor still comes up with an empty viewport so the
 * user can recover via `File → Scene → Open / New`.
 */

#include <lux/engine/editor/app/LuxEditor.hpp>
#include <lux/engine/editor/import/AssetImporter.hpp>
#include <lux/engine/editor/panels/AssetBrowser.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/launcher/SpawnHelpers.hpp>

#include <lux/engine/asset/AssetManager.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    std::filesystem::path parseProjectArg(int argc, char** argv)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            const std::string_view a = argv[i];
            if (a == "--project" || a == "-p")
                return std::filesystem::path(argv[i + 1]);
        }
        return {};
    }

    // CLI hook for M3 asset import. The user can pass
    //   --import <path-to-source-file>  [--quit-after-import]
    // to drop a .glb / .fbx / .obj / .png / etc. into the open project's
    // Content/ tree without first wiring the menu / drop UI. Useful for
    // headless verification of the import pipeline.
    //
    // Returns an empty path when `--import` is absent.
    std::filesystem::path parseImportArg(int argc, char** argv)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            const std::string_view a = argv[i];
            if (a == "--import")
                return std::filesystem::path(argv[i + 1]);
        }
        return {};
    }

    bool parseFlag(int argc, char** argv, std::string_view flag)
    {
        for (int i = 1; i < argc; ++i)
            if (std::string_view(argv[i]) == flag) return true;
        return false;
    }

    // Match either `--vk-validation` / `-vv` (implicit on) or
    // `--vk-validation=on/off/1/0/true/false` for symmetry with future
    // boolean flags. Returns the requested value, or `default_value`
    // when the flag is absent.
    bool parseVulkanValidationArg(int argc, char** argv, bool default_value)
    {
        auto parseBool = [](std::string_view v) -> bool {
            return v == "1" || v == "on" || v == "true" || v == "yes";
        };

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view a = argv[i];
            if (a == "--vk-validation" || a == "-vv")
                return true;
            constexpr std::string_view kPrefix = "--vk-validation=";
            if (a.size() > kPrefix.size() && a.substr(0, kPrefix.size()) == kPrefix)
                return parseBool(a.substr(kPrefix.size()));
        }
        return default_value;
    }
} // namespace

int main(int argc, char* argv[])
{
    const auto project_arg          = parseProjectArg(argc, argv);
    const bool vulkan_validation    = parseVulkanValidationArg(argc, argv, false);

    // ── 1. No argv: hand off to the launcher and exit immediately.
    //      Do NOT init() the editor — we don't want Vulkan to flash up.
    if (project_arg.empty())
    {
        if (!lux::launcher::spawnLauncher())
        {
            std::fprintf(stderr,
                "[lux_editor] no --project given and could not spawn "
                "lux_launcher.exe (missing next to lux_editor.exe?).\n"
                "Re-launch lux_launcher.exe manually, or pass\n"
                "  lux_editor.exe --project <path-to.luxproject>\n");
            return 1;
        }
        return 0;
    }

    // ── 2. Explicit --project: full editor session.
    std::error_code ec;
    if (!std::filesystem::exists(project_arg, ec) || ec)
    {
        std::fprintf(stderr,
            "[lux_editor] --project path does not exist: %s\n",
            project_arg.string().c_str());
        return 1;
    }

    lux::editor::EditorConfig cfg;
    cfg.title                    = "Lux Editor";
    cfg.width                    = 1600;
    cfg.height                   = 900;
    cfg.enable_vulkan_validation = vulkan_validation;

    lux::editor::LuxEditor editor(std::move(cfg));
    if (!editor.init())
    {
        std::fprintf(stderr, "[lux_editor] init failed\n");
        return 1;
    }
    if (!editor.openProject(project_arg))
    {
        // Project couldn't be parsed or its scene couldn't be loaded.
        // Don't bail — the editor stays up with an empty viewport so
        // the user can File → Open / New / Switch Project to recover.
        std::fprintf(stderr,
            "[lux_editor] failed to open project '%s'; "
            "editor is running with an empty viewport.\n",
            project_arg.string().c_str()
        );
    }

    // ── 3. Optional one-shot CLI import (M3 verification path) ──────────
    //
    // When --import <file> is on argv, run the AssetImporter against the
    // currently-open project's Content/ folder. We do this AFTER
    // openProject so we have a valid Content root to write into. The
    // editor stays up by default so we can poke around the produced
    // .luxasset files; pass --quit-after-import for headless CI use.
    const auto import_arg       = parseImportArg(argc, argv);
    const bool quit_on_import   = parseFlag(argc, argv, "--quit-after-import");
    const bool spawn_on_import  = parseFlag(argc, argv, "--spawn-after-import");
    if (!import_arg.empty())
    {
        if (!editor.currentProject())
        {
            std::fprintf(stderr,
                "[lux_editor] --import requires a successfully opened "
                "project (the importer writes into <project>/Content/).\n");
        }
        else
        {
            const auto content_root = editor.currentProject()->contentRoot();
            const auto report       = lux::editor::importExternalFile(
                import_arg, 
                content_root, 
                editor.assetManagerShared()
            );

            std::fprintf(stderr,
                "[lux_editor] import result=%d  files_written=%zu\n",
                static_cast<int>(report.result), report.written.size()
            );

            // Refresh the asset views (browser walk + registry index) so the new
            // files appear without the user having to navigate away + back.
            editor.events().content_changed.emit({});

            // Optional follow-up: auto-spawn the imported model into the
            // currently-loaded scene. Useful for "drop CesiumMan.glb in and
            // see it render" verification without a UI menu.
            if (spawn_on_import &&
                report.result == lux::editor::ImportResult::OK &&
                report.primary_asset.has_value() &&
                editor.currentScene())
            {
                const auto e = editor.spawnModelEntity(*report.primary_asset);
                std::fprintf(stderr,
                    "[lux_editor] auto-spawned entity %u from imported model\n",
                    static_cast<uint32_t>(e)
                );
            }
            if (quit_on_import) return 0;
        }
    }

    return editor.run();
}
