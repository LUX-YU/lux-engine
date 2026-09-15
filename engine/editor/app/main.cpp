#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/cxx/arguments/Arguments.hpp>
#include <cstdio>
#include <algorithm>
#include <span>
#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>
#endif

namespace
{
    lux::editor::EditorResult<std::filesystem::path> chooseProject()
    {
#if defined(_WIN32)
        std::wstring path(32768, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrFilter = L"Lux projects (*.luxproject)\0*.luxproject\0\0";
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.lpstrTitle = L"Open existing Lux project";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog))
        {
            path.resize(std::wcslen(path.data()));
            return std::filesystem::path{std::move(path)};
        }
        const auto error = CommDlgExtendedError();
        if (error)
        {
            return lux::cxx::unexpected(
                lux::editor::EditorFailure{lux::editor::EEditorError::FRONTEND_FAILURE, "project.dialog", error});
        }
        return std::filesystem::path{};
#else
        return lux::cxx::unexpected(
            lux::editor::EditorFailure{lux::editor::EEditorError::FRONTEND_FAILURE, "project.dialog", 0,
                                       "Use --project to select an existing project on this platform"});
#endif
    }
} // namespace

int main(int argc, char **argv)
{
    lux::cxx::Parser arguments("lux_editor");
    arguments.add<std::string>("project", "p").desc("Open an existing .luxproject");
    arguments.add<std::string>("font").desc("Explicit cold UI font file");
    const auto parsed = arguments.parse(argc, argv);
    if (!parsed)
    {
        std::fprintf(stderr, "%s\n%s", lux::cxx::to_string(parsed.error()).data(), arguments.usage().c_str());
        return 2;
    }
    // lux-cxx reserves -h/--help and deliberately excludes them from ParsedOptions.
    if (std::ranges::any_of(std::span{argv + 1, static_cast<std::size_t>(argc - 1)},
                            [](std::string_view value) { return value == "--help" || value == "-h"; }))
    {
        std::printf("%s", arguments.usage().c_str());
        return 0;
    }
    lux::editor::EditorConfig config;
    if (parsed->contains("project"))
    {
        config.project_file = std::filesystem::u8path(*parsed->get("project").as<std::string>());
    }
    else
    {
        auto project = chooseProject();
        if (!project)
        {
            std::fprintf(stderr, "%s:%llu %s\n", project.error().domain.c_str(), project.error().reason,
                         project.error().message.c_str());
            return 2;
        }
        if (project->empty())
        {
            return 0;
        }
        config.project_file = std::move(*project);
    }
    lux::editor::gui::GuiConfig gui;
    if (parsed->contains("font"))
    {
        gui.window.font.emplace();
        gui.window.font->file = std::filesystem::u8path(*parsed->get("font").as<std::string>());
    }
    gui.providers.push_back(lux::editor::gui::sceneDocumentProvider());
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    config.frontend = [gui = std::move(gui)] { return lux::editor::gui::makeGuiFrontend(gui); };
    lux::meta::ReflectionRegistry::initRegistry();
    lux::editor::Editor editor(std::move(config));
    return editor.exec();
}
