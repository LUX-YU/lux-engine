#include <algorithm>
#include <cstdio>
#include <lux/cxx/arguments/Arguments.hpp>
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/editor/gui/flowforge/FlowForgeDocumentProvider.hpp>
#include <lux/engine/editor/gui/material/MaterialDocumentProvider.hpp>
#include <lux/engine/editor/gui/scene/SceneDocumentProvider.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <span>
#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>
#endif

namespace
{
    // The registry owns generated metadata; the immutable selection owns its pointer arrays.
    // Frontend, open requests, documents and worker captures share this lease until their last use.
    struct ReflectedFlowMetadata final
    {
        std::vector<const lux::meta::RefClass *> classes;
        std::vector<const lux::meta::RefFunction *> functions;

        ReflectedFlowMetadata()
        {
            lux::meta::ReflectionRegistry::initRegistry();
            const auto &registry = lux::meta::ReflectionRegistry::instance();
            for (const auto &type : registry.classes())
            {
                if (type && type->type.size != 0)
                {
                    classes.push_back(type.get());
                }
            }
            for (const auto &function : registry.functions())
            {
                if (function)
                {
                    functions.push_back(function.get());
                }
            }
        }
        ~ReflectedFlowMetadata()
        {
            lux::meta::ReflectionRegistry::destroyRegistry();
        }
        ReflectedFlowMetadata(const ReflectedFlowMetadata &) = delete;
        ReflectedFlowMetadata &operator=(const ReflectedFlowMetadata &) = delete;
    };

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
    auto metadata = std::make_shared<ReflectedFlowMetadata>();
    lux::flowforge::FlowSourceEnvironment flow;
    flow.classes = metadata->classes;
    flow.functions = metadata->functions;
    flow.code_lifetime = metadata;
    gui.providers.push_back(lux::editor::gui::sceneDocumentProvider());
    gui.providers.push_back(lux::editor::gui::materialDocumentProvider());
    gui.providers.push_back(lux::editor::gui::flowForgeDocumentProvider(std::move(flow)));
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    config.frontend = [gui = std::move(gui)] { return lux::editor::gui::makeGuiFrontend(gui); };
    lux::editor::Editor editor(std::move(config));
    return editor.exec();
}
