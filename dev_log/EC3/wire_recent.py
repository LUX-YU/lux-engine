from pathlib import Path
import json
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p = s / 'editor/application/src/EditorProjectTools.cpp'
text = p.read_text()
a = text.index('    void EditorApplication::Impl::maintainRecentProjects()')
b = text.index('    void EditorApplication::Impl::installRecentProjects', a)
text = text[:a] + text[b:]
a = text.index('        class RecentPane final')
b = text.index('        draft.views.push_back(', a)
text = text[:a] + text[b:]
old = '''                return views::DetachedView{
                    contracts::CodeLease::builtin(),
                    std::make_unique<RecentPane>(input.dispatcher(), input.paneId(), *this)
                };'''
new = '''                std::erase_if(connections_, [](const auto& connection) { return !connection.connected(); });
                if (connections_.size() >= 64)
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::CONSTRUCT, "recent.connections"
                    });
                auto view = std::make_unique<project::RecentProjectsView>(
                    input.dispatcher(), input.paneId(), *recent_projects_
                );
                auto connected = object::LuxObject::connect(
                    view.get(), &project::RecentProjectsView::openRequested,
                    [this, receiver = view.get()](const std::filesystem::path& path) noexcept {
                        const bool is_unavailable = phase_ != EApplicationPhase::RUNNING ||
                            project_launch_.has_value() || project_launch_intent_.has_value();
                        if (is_unavailable)
                            receiver->showFailure({EEditorError::BUSY, "recent.open"});
                        else
                            project_launch_intent_ = path;
                    }
                );
                if (!connected)
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::CONSTRUCT, "recent.connect"
                    });
                connections_.push_back(std::move(*connected));
                return views::DetachedView{contracts::CodeLease::builtin(), std::move(view)};'''
assert old in text
text = text.replace(old, new)
text = text.replace('#include <lux/engine/editor/project/ImportView.hpp>', '#include <lux/engine/editor/project/ImportView.hpp>\n#include <lux/engine/editor/project/RecentProjectsView.hpp>')
for header in ['lux/engine/editor/storage/FilePublication.hpp', 'lux/engine/ui/Element.hpp', 'toml++/toml.hpp', 'imgui.h', 'sstream']:
    text = text.replace('#include <' + header + '>\n', '')
p.write_text(text)
p = s / 'editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp'
text = p.read_text()
a = text.index('        struct RecentProjects final')
b = text.index('        std::optional<process::TaskId> project_launch_', a)
text = text[:a] + '        std::unique_ptr<RecentProjects> recent_projects_;\n' + text[b:]
text = text.replace('#include <lux/engine/editor/storage/ProjectPluginSelection.hpp>', '#include <lux/engine/editor/storage/ProjectPluginSelection.hpp>\n#include <lux/engine/editor/storage/RecentProjects.hpp>')
text = text.replace('        void maintainRecentProjects();\n', '')
p.write_text(text)
p = s / 'editor/application/src/EditorLifecycle.cpp'
text = p.read_text().replace('        maintainRecentProjects();', '        receive(recent_projects_->update(phase_ == EApplicationPhase::RUNNING));')
text = text.replace('opening_.settled() && !recent_task_ &&\n            !recent_result_ && !recent_ticket_ && !project_launch_', 'opening_.settled() && recent_projects_->settled() && !project_launch_')
p.write_text(text)
p = s / 'editor/application/src/EditorApplication.cpp'
text = p.read_text().replace('        importer_ =\n', '''        recent_projects_ = std::make_unique<RecentProjects>(
            *config_.user_directory, config_.project_file, engine_->execution(), writes_, files_, save_execution_
        );
        importer_ =
''')
p.write_text(text)
p = s / 'editor/activities/project/CMakeLists.txt'
text = p.read_text().replace('src/ProjectPluginSelection.cpp\n', 'src/ProjectPluginSelection.cpp src/RecentProjects.cpp\n')
text = text.replace('    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectPluginSelection.hpp', '    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/RecentProjects.hpp\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectPluginSelection.hpp')
p.write_text(text)
p = s / 'editor/workbench/project/tools/CMakeLists.txt'
text = p.read_text().replace('SOURCE_FILES src/ImportView.cpp', 'SOURCE_FILES src/RecentProjectsView.cpp src/ImportView.cpp')
p.write_text(text)
p = s / 'editor/tests/architecture/rules.json'
data = json.loads(p.read_text())
def find(mapping):
    if isinstance(mapping, dict):
        if 'editor/activities/project/src/ProjectContentSaving.cpp' in mapping: return mapping
        for value in mapping.values():
            result = find(value)
            if result is not None: return result
    return None
providers = find(data)
assert providers is not None
for name, provider in [
    ('editor/activities/project/include/lux/engine/editor/storage/RecentProjects.hpp', 'editor_storage'),
    ('editor/activities/project/src/RecentProjects.cpp', 'editor_storage'),
    ('editor/workbench/project/tools/include/lux/engine/editor/project/RecentProjectsView.hpp', 'project_tools_ui'),
    ('editor/workbench/project/tools/src/RecentProjectsView.cpp', 'project_tools_ui')
]:
    providers[name] = [provider]
p.write_text(json.dumps(data, indent=2) + '\n')
