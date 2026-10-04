from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
impl=s/'editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp'
it=impl.read_text()
helper=it[it.index('    template <class Error> auto applicationFailure'):it.index('    struct EditorApplication::Impl')]
helper=helper.replace('applicationFailure','failure')
views=s/'editor/application/src/EditorViews.cpp'
vt=views.read_text()
a=vt.index('        auto resolved = project_->resolveReference', vt.index('EditorApplication::Impl::openCaptured'))
b=vt.index('        opens_.push_back({*opened});',a)
opening=vt[a:b]+'        return *opened;\n'
opening=opening.replace('project_->','project.').replace('files_.','files.').replace('opening_.','opening.')
opening=opening.replace('snapshot.sessions()', 'snapshot').replace('applicationFailure(', 'openingFailure(')
opening=opening.replace('''        auto factory = snapshot.selectSource(asset->source_type, asset->source_version);''','''        const auto entry = *asset;
        auto factory = snapshot.selectSource(entry.source_type, entry.source_version);''')
opening=opening.replace('asset->source_path','entry.source_path').replace('asset->id','entry.id')
opening=opening.replace('''        auto source = project.captureSource''','''        // Resolving an external backend can invoke code. Revalidate the original catalog reference
        // before capturing source bytes, rather than retaining a catalog pointer across that call.
        if (auto current = project.resolveReference(reference, 0); !current)
            return cxx::unexpected(current.error());
        auto source = project.captureSource''')
asset=s/'editor/activities/project/src/ProjectAssetSource.cpp'
at=asset.read_text().replace('#include <lux/engine/editor/storage/ProjectStorage.hpp>', '#include <lux/engine/editor/storage/ProjectStorage.hpp>\n#include <lux/engine/editor/storage/ProjectContentOpening.hpp>\n#include <lux/engine/editor/persistence/ArtifactStore.hpp>')
at+='\nnamespace lux::editor\n{\n    namespace\n    {\n'+helper.replace('failure','openingFailure')+'''    }
    EditorResult<sessions::OpenAssetId> openProjectContent(
        ProjectStorage& project, persistence::IArtifactStore& files, sessions::SessionOpening& opening,
        AssetReference reference, const sessions::SessionFactorySnapshot& snapshot
    )
    {
'''+opening+'    }\n}\n'
asset.write_text(at)
vt=vt[:a]+'''        auto opened = openProjectContent(*project_, files_, opening_, reference, snapshot.sessions());
        if (!opened)
            return cxx::unexpected(opened.error());
'''+vt[b:]
vt=vt.replace('#include <lux/engine/scene/RenderSystem.hpp>', '#include <lux/engine/scene/RenderSystem.hpp>\n#include <lux/engine/editor/storage/ProjectContentOpening.hpp>')
views.write_text(vt)

p=s/'editor/application/src/EditorRecovery.cpp'
t=p.read_text()
namespace=t[t.index('    namespace\n'):t.index('    EditorResult<void> EditorApplication::Impl::captureRecovery')]
ca=t[t.index('    EditorResult<void> EditorApplication::Impl::captureRecovery'):t.index('    EditorResult<void> EditorApplication::Impl::restoreRecovery')]
ca=ca.replace('EditorApplication::Impl::captureRecovery()', 'captureCore(\n        std::span<const views::ViewInfo> views, const extensions::ContributionSnapshot& catalog\n    )')
a=ca.index('        auto views = desktop_->views().describeAll();')
b=ca.index('        for (const auto& view : *views)',a)
ca=ca[:a]+ca[b:].replace('for (const auto& view : *views)','for (const auto& view : views)',1)
start=t[t.index('    EditorResult<void> EditorApplication::Impl::restoreRecovery'):t.index('    EditorResult<void> EditorApplication::Impl::settleRecovery')]
start=start.replace('EditorApplication::Impl::restoreRecovery()', 'startCore()')
update=t[t.index('    EditorResult<void> EditorApplication::Impl::settleRecovery'):].rstrip()
assert update.endswith('}')
update=update[:-1]
update=update.replace('EditorApplication::Impl::settleRecovery()', 'updateCore(ERestorationProgress progress, Present present)')
update=update.replace('                std::erase_if(opens_, [&](const auto& entry) { return entry.operation == *item.opening; });\n','')
update=update.replace('''const bool is_reviewing = phase_ == EApplicationPhase::REVIEWING ||
                                          phase_ == EApplicationPhase::COMMITTING_EXIT;''','''const bool is_reviewing = progress == ERestorationProgress::SUSPENDED;''')
update=update.replace('phase_ != EApplicationPhase::RUNNING','progress != ERestorationProgress::ACTIVE').replace('phase_ == EApplicationPhase::DRAINING','progress == ERestorationProgress::CLOSING')
update=update.replace('project_->', 'project_.')
update=update.replace('admitted.emplace(openCaptured(project_.reference(asset_id), recovery_->catalog));','''admitted.emplace(openProjectContent(
                        project_, files_, opening_, project_.reference(asset_id), recovery_->catalog.sessions()
                    ));''')
a=update.index('                auto presentation = std::ranges::find(opens_')
b=update.index('                continue;',a)
update=update[:a]+update[b:]
update=update.replace('''displayed.emplace(makeContentView(
                    association, true, recovery_->catalog, item.entry.restore_key, recoveryType(item.entry.type)
                ));''','''displayed.emplace(present(
                    association, recovery_->catalog, item.entry.restore_key, recoveryType(item.entry.type)
                ));''')
methods='\n'.join('    '+line if line else '' for line in (ca+start+update).splitlines())
methods=methods.replace('applicationFailure(', 'failure(')
src='''#include <lux/engine/editor/application/RestoreWorkbench.hpp>
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/storage/ProjectContentOpening.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/editor/views/ViewFactory.hpp>
#include <algorithm>
#include <thread>

namespace lux::editor::application
{
'''+namespace+'    namespace\n    {\n'+helper+'''    }
    struct RestoreWorkbench::Impl final
    {
        struct RecoveryPresentation final
        {
            extensions::ContributionSnapshot catalog;
            std::vector<RestoredView> items;
        };
        ProjectStorage& project_;
        persistence::IArtifactStore& files_;
        sessions::SessionStore& sessions_;
        sessions::SessionOpening& opening_;
        workspace::WorkspaceStore& workspace_;
        workspace::WorkspaceChanges& workspace_changes_;
        extensions::ContributionRegistry& contributions_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        std::optional<RecoveryPresentation> recovery_;
        Impl(
            ProjectStorage& project, persistence::IArtifactStore& files, sessions::SessionStore& sessions,
            sessions::SessionOpening& opening, workspace::WorkspaceStore& workspace,
            workspace::WorkspaceChanges& changes, extensions::ContributionRegistry& contributions
        ) : project_(project), files_(files), sessions_(sessions), opening_(opening), workspace_(workspace),
            workspace_changes_(changes), contributions_(contributions)
        {}
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value) { active = true; }
            ~Dispatch() { active = false; }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
        EditorResult<void> admission() const
        {
            if (owner_ != std::this_thread::get_id())
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "recovery.owner-thread"});
            if (dispatching_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "recovery.dispatch"});
            return {};
        }
'''+methods+'''
    };
    RestoreWorkbench::RestoreWorkbench(
        ProjectStorage& project, persistence::IArtifactStore& files, sessions::SessionStore& sessions,
        sessions::SessionOpening& opening, workspace::WorkspaceStore& workspace, workspace::WorkspaceChanges& changes,
        extensions::ContributionRegistry& contributions
    ) : impl_(std::make_unique<Impl>(project, files, sessions, opening, workspace, changes, contributions))
    {}
    RestoreWorkbench::~RestoreWorkbench() = default;
    EditorResult<void> RestoreWorkbench::capture(std::span<const views::ViewInfo> views)
    {
        if (auto ready = impl_->admission(); !ready)
            return ready;
        const Impl::Dispatch scope{impl_->dispatching_};
        EditorResult<void> result;
        auto capture = [&](const extensions::ContributionSnapshot& catalog) -> extensions::ContributionResult<void> {
            result = impl_->captureCore(views, catalog);
            return {};
        };
        auto guarded = impl_->contributions_.withSnapshot(capture);
        return guarded ? std::move(result) : failure("recovery.catalog", guarded.error());
    }
    EditorResult<void> RestoreWorkbench::start()
    {
        if (auto ready = impl_->admission(); !ready)
            return ready;
        const Impl::Dispatch scope{impl_->dispatching_};
        return impl_->startCore();
    }
    EditorResult<void> RestoreWorkbench::update(ERestorationProgress progress, Present present)
    {
        if (auto ready = impl_->admission(); !ready)
            return ready;
        const Impl::Dispatch scope{impl_->dispatching_};
        return impl_->updateCore(progress, present);
    }
    std::span<const RestoredView> RestoreWorkbench::items() const noexcept
    {
        return impl_->recovery_ ? std::span<const RestoredView>{impl_->recovery_->items} : std::span<const RestoredView>{};
    }
}
'''
(s/'editor/application/src/RestoreWorkbench.cpp').write_text(src)
p.write_text('''#include <lux/engine/editor/application/EditorApplicationImpl.hpp>

namespace lux::editor::application
{
    EditorResult<void> EditorApplication::Impl::captureRecovery()
    {
        auto views = desktop_->views().describeAll();
        if (!views)
            return applicationFailure("recovery.views", views.error());
        return restoration_->capture(*views);
    }
    EditorResult<void> EditorApplication::Impl::restoreRecovery()
    {
        return restoration_->start();
    }
    EditorResult<void> EditorApplication::Impl::settleRecovery()
    {
        auto progress = ERestorationProgress::SUSPENDED;
        if (phase_ == EApplicationPhase::RUNNING)
            progress = ERestorationProgress::ACTIVE;
        else if (phase_ == EApplicationPhase::DRAINING)
            progress = ERestorationProgress::CLOSING;
        auto present = [&](views::ViewContent content, const extensions::ContributionSnapshot& catalog,
                           views::ViewRestoreKey key, views::ViewTypeId type) {
            return makeContentView(std::move(content), true, catalog, std::move(key), std::move(type));
        };
        return restoration_->update(progress, present);
    }
}
''')
it=it.replace('#pragma once', '#pragma once\n#include <lux/engine/editor/application/RestoreWorkbench.hpp>',1)
a=it.index('        struct RecoveryItem final');b=it.index('        struct Dispatch final',a)
it=it[:a]+it[b:]
it=it.replace('        std::optional<RecoveryPresentation> recovery_;','        std::unique_ptr<RestoreWorkbench> restoration_;')
impl.write_text(it)
p=s/'editor/application/src/EditorApplication.cpp';t=p.read_text();needle='        auto contributions = installContributions();'
t=t.replace(needle,'''        restoration_ = std::make_unique<RestoreWorkbench>(
            *project_, files_, sessions_, opening_, workspace_, workspace_changes_, contributions_
        );
'''+needle);p.write_text(t)
p=s/'editor/application/src/EditorWorkspace.cpp';t=p.read_text()
t=t.replace('        if (recovery_)\n            for (const auto& item : recovery_->items)','        for (const auto& item : restoration_->items())')
p.write_text(t)
p=s/'editor/tests/integration/application/application.cpp';t=p.read_text().replace('impl.recovery_->items','impl.restoration_->items()');p.write_text(t)
p=s/'editor/application/CMakeLists.txt';t=p.read_text().replace('src/EditorRecovery.cpp ','src/EditorRecovery.cpp src/RestoreWorkbench.cpp ');p.write_text(t)
p=s/'editor/activities/project/CMakeLists.txt';t=p.read_text().replace('    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectContentSaving.hpp','    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectContentOpening.hpp\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectContentSaving.hpp');p.write_text(t)
p=s/'editor/tests/architecture/rules.json';d=json.loads(p.read_text())
def find(mapping):
    if isinstance(mapping,dict):
        if 'editor/activities/project/src/ProjectContentSaving.cpp' in mapping:return mapping
        for v in mapping.values():
            r=find(v)
            if r is not None:return r
providers=find(d)
for f,target in [('editor/activities/project/include/lux/engine/editor/storage/ProjectContentOpening.hpp','editor_storage'),('editor/application/pinclude/lux/engine/editor/application/RestoreWorkbench.hpp','editor_bootstrap'),('editor/application/src/RestoreWorkbench.cpp','editor_bootstrap')]:providers[f]=[target]
p.write_text(json.dumps(d,indent=2)+'\n')
