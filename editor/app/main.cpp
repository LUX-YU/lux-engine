#include <algorithm>
#include <cstdio>
#include <lux/cxx/arguments/Arguments.hpp>
#include <lux/engine/editor/Editor.hpp>
#include "product/ProductAssembly.hpp"
#include <span>
#include <lux/engine/window/FileDialog.hpp>
#include <lux/engine/platform/Process.hpp>

int main(int argc, char** argv)
{
    auto utf8 = lux::engine::platform::processArguments(argc, argv);
    if (!utf8)
        return 2;
    std::vector<char*> pointers;
    for (auto& argument : *utf8)
        pointers.push_back(argument.data());
    argc = static_cast<int>(pointers.size());
    argv = pointers.data();
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
    if (std::ranges::any_of(std::span{argv + 1, static_cast<std::size_t>(argc - 1)}, [](std::string_view value) {
            return value == "--help" || value == "-h";
        }))
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
        const std::array filters{lux::window::FileDialogFilter{"Lux project", "luxproject"}};
        auto project = lux::window::openFileDialog(nullptr, filters);
        if (!project)
        {
            std::fprintf(stderr, "project.dialog: %s\n", project.error().detail.c_str());
            return 2;
        }
        if (!*project)
        {
            return 0;
        }
        config.project_file = std::move(**project);
    }
    if (parsed->contains("font"))
    {
        config.window.font.emplace();
        config.window.font->file = std::filesystem::u8path(*parsed->get("font").as<std::string>());
    }
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    // Installation paths are anchored to the executable, independent of the project and CWD.
    auto executable = lux::engine::platform::executablePath();
    if (!executable)
        return 2;
    config.plugin_root = executable->parent_path().parent_path();

    auto editor = lux::editor::Editor::create(std::move(config), &lux::editor::assembleProduct);
    if (!editor)
    {
        std::fprintf(stderr, "%s: %s\n", editor.error().domain.c_str(), editor.error().message.c_str());
        return 3;
    }
    return (*editor)->exec();
}
