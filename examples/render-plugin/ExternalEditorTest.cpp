#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <cassert>
#include <chrono>
#include <fstream>
#include <cstdio>

int main(int argc, char** argv)
{
    using namespace lux::editor;
    assert(argc == 2);
    const auto root = std::filesystem::path(argv[1]) / "editor-test" /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    ProjectManifest project;
    std::array<std::uint8_t, 16> id{};
    id.back() = 1;
    project.id = lux::asset::AssetId{id};
    project.name = "External editor";
    project.plugins.push_back({"sample.render.triangle", 1, {}});
    const auto encoded = encodeProjectManifest(project);
    assert(encoded);
    { std::ofstream output(root / "Project.luxproject", std::ios::binary); output << *encoded; assert(output); }
    EditorConfig config;
    config.project_file = root / "Project.luxproject";
    config.plugin_root = argv[1];
    config.window.visible = false;
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    auto editor = Editor::create(std::move(config));
    if (!editor) std::fprintf(stderr, "%s: %s\n", editor.error().domain.c_str(), editor.error().message.c_str());
    assert(editor);
    assert((*editor)->context().panes().registrations().size() == 1);
    std::weak_ptr<const void> code;
    for (const auto& registration : (*editor)->context().panes().registrations())
        if (registration.type.name() == "sample.editor") code = registration.code_lifetime;
    assert(!code.expired());
    const auto first = (*editor)->context().panes().create(lux::ui::PaneTypeIdView{"sample.editor"});
    const auto second = (*editor)->context().panes().create(lux::ui::PaneTypeIdView{"sample.editor"});
    assert(first && second && &first->get() == &second->get());
    (*editor)->requestExit();
    assert((*editor)->exec() == 0);
    editor->reset();
    assert(code.expired());
    std::puts("PASS installed plugin supplies an editor, reuse, close events and code retirement");
}
