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
 *
 * Argument parsing is `lux::cxx::arguments` (same parser the asset tool
 * uses) — the option table below IS the usage text. Two consequences
 * differ from the hand-rolled scan this replaced, both deliberate:
 *   • unknown options are now REJECTED (they used to be ignored, so a
 *     typo'd `--porject` silently fell through to "spawn the launcher");
 *   • `--vk-validation=<value>` accepts 1/0/true/false/yes/no. The
 *     previously-documented `on`/`off` spellings are gone — the shared
 *     parser's bool grammar does not have them, and nothing in-tree
 *     used that form. A value it cannot read is a hard error, never a
 *     silent `false`.
 */

#include <lux/engine/editor/app/LuxEditor.hpp>
#include <lux/engine/editor/import/AssetImporter.hpp>
#include <lux/engine/editor/panels/AssetBrowser.hpp>
#include <lux/engine/authoring/project/Project.hpp>
#include <lux/engine/hosts/launcher/SpawnHelpers.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/AssetHeaderProbe.hpp>
#include <lux/engine/log/Log.hpp>

#include <lux/cxx/arguments/Arguments.hpp>

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
    /// `Parser::parse` SKIPS `-h` / `--help` (they never reach ParsedOptions),
    /// so the caller has to spot them itself — otherwise `lux_editor --help`
    /// would parse clean, find no `--project`, and spawn the launcher.
    bool wantsHelp(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view a = argv[i];
            if (a == "--help" || a == "-h") return true;
        }
        return false;
    }

    lux::cxx::Parser buildParser()
    {
        lux::cxx::Parser parser("lux_editor");

        parser.add<std::string>("project", "p")
            .desc("The .luxproject manifest to open (absent = spawn the launcher)");

        parser.add<bool>("vk-validation", "vv")
            .desc("Enable the Vulkan validation layer (messages go to stderr)")
            .def(false);

        // CLI hook for M3 asset import: drop a .glb / .fbx / .obj / .png into
        // the open project's Content/ tree without first wiring the menu / drop
        // UI. Useful for headless verification of the import pipeline.
        parser.add<std::string>("import")
            .desc("One-shot: import this source file into <project>/Content/ at startup");

        parser.add<bool>("quit-after-import")
            .desc("Exit as soon as --import finishes (headless CI use)")
            .def(false);

        parser.add<bool>("spawn-after-import")
            .desc("After --import, spawn the imported model into the loaded scene")
            .def(false);

        return parser;
    }
} // namespace

int main(int argc, char* argv[])
{
    const lux::cxx::Parser parser = buildParser();

    if (wantsHelp(argc, argv))
    {
        std::fputs(parser.usage().c_str(), stdout);
        return 0;
    }

    const auto parsed = parser.parse(argc, argv);
    if (!parsed)
    {
        const auto why = lux::cxx::to_string(parsed.error());
        std::fprintf(stderr, "[lux_editor] %.*s\n",
                     static_cast<int>(why.size()), why.data());
        std::fputs(parser.usage().c_str(), stderr);
        return 2;
    }
    const auto& opts = *parsed;

    // `.def(false)` means the option is always present, so a read only fails
    // when the user wrote a value the bool grammar cannot read. Say so instead
    // of falling back to false — a silently-ignored `--vk-validation=maybe`
    // is exactly the kind of "it didn't take and nobody noticed" this parser
    // switch is meant to end.
    const auto validation = opts.get("vk-validation").as<bool>();
    if (!validation)
    {
        const auto why = lux::cxx::to_string(validation.error());
        std::fprintf(stderr,
            "[lux_editor] --vk-validation: %.*s (accepted: bare flag, or "
            "=1/0/true/false/yes/no)\n",
            static_cast<int>(why.size()), why.data());
        return 2;
    }
    const bool vulkan_validation = *validation;

    const std::filesystem::path project_arg =
        opts.get("project").as<std::string>().value_or(std::string{});

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

    // 日志出口装配(事件批C)移进 LuxEditor::init:单出口发布进统一事件总线,
    // stderr/toast 是泵订阅者。init 之前的极早期日志走 log 模块的 stderr
    // 回落 —— 行形状一致,忘记装配不得静默的纪律不变。
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
    // We do this AFTER openProject so we have a valid Content root to write
    // into. The editor stays up by default so we can poke around the produced
    // .luxasset files; pass --quit-after-import for headless CI use.
    const std::filesystem::path import_arg =
        opts.get("import").as<std::string>().value_or(std::string{});
    const bool quit_on_import  = opts.get("quit-after-import").as<bool>().value_or(false);
    const bool spawn_on_import = opts.get("spawn-after-import").as<bool>().value_or(false);
    bool quit_after_spawn_completion = false;
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

            for (const auto& path : report.written)
            {
                const auto probe = lux::asset::readAssetHeader(path);
                if (probe.id.is_nil()) continue;
                editor.events().publish(lux::editor::EditorAssetChanged{
                    probe.id,
                    lux::editor::EEditorAssetChange::ADDED,
                    editor.assetManagerShared()->contentRevision(probe.id),
                    path
                });
            }

            // Optional follow-up: auto-spawn the imported model into the
            // currently-loaded scene. Useful for "drop CesiumMan.glb in and
            // see it render" verification without a UI menu.
            if (spawn_on_import &&
                report.result == lux::editor::ImportResult::OK &&
                report.primary_asset.has_value() &&
                editor.currentScene())
            {
                const auto submitted = editor.spawnModelEntity(
                    *report.primary_asset,
                    [&editor, quit_on_import](
                        lux::editor::InstanceSpawnOutcome outcome)
                    {
                        if (outcome)
                        {
                            std::fprintf(
                                stderr,
                                "[lux_editor] auto-spawned entity %u from "
                                "imported model\n",
                                static_cast<std::uint32_t>(outcome->root));
                        }
                        else
                        {
                            std::fprintf(
                                stderr,
                                "[lux_editor] async spawn failed (stage=%d)\n",
                                static_cast<int>(outcome.error().code));
                        }
                        if (quit_on_import)
                            editor.requestQuit();
                    });
                if (!submitted)
                {
                    std::fprintf(
                        stderr,
                        "[lux_editor] async spawn rejected (runtime=%d)\n",
                        static_cast<int>(submitted.error()));
                }
                else if (quit_on_import)
                    quit_after_spawn_completion = true;
            }
            if (quit_on_import && !quit_after_spawn_completion)
                return 0;
        }
    }

    return editor.run();
}
