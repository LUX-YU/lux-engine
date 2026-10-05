#include <lux/engine/editor/project/ProjectModule.hpp>
#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <cassert>
#include <chrono>
#include <fstream>
#include <cstdio>

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor;
    assert(argc == 2);
    const auto installation = std::filesystem::path(argv[1]);
    const auto root =
        installation / "editor-test" / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    ProjectManifest project;
    std::array<std::uint8_t, 16> id{};
    id.back() = 1;
    project.id = asset::AssetId{id};
    project.name = "External editor";
    project.plugins.push_back({"sample.render.triangle", 1, {}});
    const auto encoded = encodeProjectManifest(project);
    assert(encoded);
    {
        std::ofstream output(root / "Project.luxproject", std::ios::binary);
        output << *encoded;
        assert(output);
    }
    application::EditorApplicationConfig config;
    config.project_file = root / "Project.luxproject";
    config.installation = installation;
    config.width = 1100;
    config.height = 820;
    config.offscreen = true;
    config.user_directory = root;
    auto editor = application::EditorApplication::create(std::move(config), std::array{&project::projectModule});
    if (!editor)
        std::fprintf(stderr, "%s: %s\n", editor.error().domain.c_str(), editor.error().message.c_str());
    assert(editor);
    // A contributed standalone factory is exposed through the real Window menu command.
    // Repeating it focuses the same tool; duplicate IDs would be rejected by Host.
    for (unsigned i{}; i != 2; ++i)
    {
        assert((*editor)->execute(commands::CommandId{"lux.editor.tool/sample.editor"}));
        assert((*editor)->update());
    }
    assert((*editor)->requestExit());
    assert((*editor)->exec());
    assert((*editor)->phase() == application::EApplicationPhase::RELEASED);
    editor->reset();
    std::puts("PASS installed V8 plugin: formal application Window command, reuse, close and retirement");
}
