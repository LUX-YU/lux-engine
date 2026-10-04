from pathlib import Path
root=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
s=(root/'editor/application/src/EditorSaving.cpp').read_text()
def method(name):
 a=s.index('    EditorResult<', s.index('    EditorResult<') if name=='prepareSave' else s.index('EditorApplication::Impl::'+name)-100)
 # Locate the function declaration by its exact owner then preceding line.
 owner=s.index('EditorApplication::Impl::'+name+'(')
 a=s.rfind('\n    EditorResult<',0,owner)+1
 b=s.index('\n    }',owner)+len('\n    }')
 return s[a:b]
prepare=method('prepareSave').replace('EditorApplication::Impl::PreparedSave','PreparedProjectSave').replace('EditorApplication::Impl::prepareSave','prepare').replace('commands::SessionTarget target','sessions::ContentStamp source').replace('target.id','source.session').replace('!target.based_on || *target.based_on != info->current','source != info->current').replace('PreparedSave{','PreparedProjectSave{')
prepare=prepare.replace('persistence::SaveRequest request{source.session, mode};','persistence::SaveRequest request{source.session, mode};\n        request.based_on = source;')
request=method('save').replace('EditorApplication::Impl::save','request').replace('commands::SessionTarget target','sessions::ContentStamp source')
request=request.replace('        if (auto ended = cancelContentPreview(target.id); !ended)\n            return cxx::unexpected(ended.error());\n','').replace('prepareSave(target, mode','prepare(source, mode')
track=method('rememberSave').replace('EditorApplication::Impl::rememberSave(persistence::SaveId id)','track(persistence::SaveId id, std::span<const ProjectAssetEntry> destinations = {})').replace('close_destinations_','destinations')
# Record-capacity admission occurs before accepted metadata can be appended.
track=track.replace('        auto status = saves_.status(id);','        if (save_reports_.size() >= capacity_)\n            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "save.reports"});\n        auto status = saves_.status(id);',1)
track=track.replace('                    save_reports_.push_back({id, entry});','                    save_reports_.emplace_back(id, entry);\n                    pending_saves_.push_back(id);')
update=method('settleSaves').replace('EditorApplication::Impl::settleSaves()','update(std::span<const sessions::SaveAllEntry> borrowed)')
update=update.replace('            auto remembered = rememberSave(id);\n            if (!remembered)\n                return remembered;\n','')
update=update.replace('closing_ && std::ranges::any_of(closing_->saves(),','std::ranges::any_of(borrowed,')
update=update.replace('    {\n        for (auto iterator', '''    {
        if (save_all_)
        {
            const auto entries = save_all_->entries();
            while (save_all_tracked_ < entries.size())
            {
                if (entries[save_all_tracked_].save)
                    if (auto accepted = track(*entries[save_all_tracked_].save); !accepted)
                        return accepted;
                ++save_all_tracked_;
            }
        }
        for (auto iterator''',1)
common=prepare+'\n'+request+'\n'+track+'\n'+update
common=common.replace('project_->','project_.').replace('applicationFailure(', 'failure(').replace('SavePresentation::','ProjectSaveReport::')
common=common.replace('report.catalog)', 'report.catalog_)').replace('report.catalog->','report.catalog_->').replace('report.catalog =','report.catalog_ =').replace('*report.catalog,','*report.catalog_,').replace('report.catalog.reset()', 'report.catalog_.reset()')
common=common.replace('save_reports_.push_back({*accepted, std::move(prepared->asset)});','save_reports_.emplace_back(*accepted, std::move(prepared->asset));').replace('>= 128','>= capacity_')
source='''#include <lux/engine/editor/storage/ProjectContentSaving.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/sessions/SessionOperations.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <algorithm>
#include <random>
#include <thread>

namespace lux::editor
{
    namespace
    {
        template<class Error> auto failure(std::string domain, const Error& cause)
        {
            auto code = EEditorError::SOURCE_FAILURE;
            if constexpr (requires { cause.code == decltype(cause.code)::BUSY; })
            {
                if (cause.code == decltype(cause.code)::BUSY)
                    code = EEditorError::BUSY;
            }
            else if constexpr (requires { cause == Error::BUSY; })
            {
                if (cause == Error::BUSY)
                    code = EEditorError::BUSY;
            }
            return cxx::unexpected(EditorFailure{code, std::move(domain), 0, {}, cause});
        }
    }
    ProjectSaveReport::ProjectSaveReport(persistence::SaveId value, ProjectAssetEntry entry)
        : id(value), asset(std::move(entry))
    {}
    struct ProjectContentSaving::Impl final
    {
        static constexpr std::size_t capacity_ = 128;
        sessions::SessionStore& sessions_;
        sessions::SessionOpening& opening_;
        persistence::SaveService& saves_;
        ProjectStorage& project_;
        persistence::WriteCoordinator& writes_;
        persistence::IArtifactStore& files_;
        std::vector<ProjectSaveReport> save_reports_;
        std::vector<persistence::SaveId> pending_saves_;
        std::optional<sessions::SaveAllOperation> save_all_;
        std::size_t save_all_tracked_{};
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) : active(value) { active = true; }
            ~Dispatch() { active = false; }
        };
        Impl(sessions::SessionStore& sessions, sessions::SessionOpening& opening,
             persistence::SaveService& saves, ProjectStorage& project,
             persistence::WriteCoordinator& writes, persistence::IArtifactStore& files)
            : sessions_(sessions), opening_(opening), saves_(saves), project_(project), writes_(writes), files_(files)
        {
            save_reports_.reserve(capacity_);
            pending_saves_.reserve(capacity_);
        }
        EditorResult<void> admission() const
        {
            if (owner_ != std::this_thread::get_id())
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "save.owner-thread"});
            if (dispatching_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "save.dispatch"});
            return {};
        }
        EditorResult<void> saveAll()
        {
            if (save_all_ && save_all_tracked_ != save_all_->entries().size())
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "save-all.association"});
            auto ids = sessions_.snapshotIds();
            if (!ids)
                return failure("save-all.contents", ids.error());
            if (ids->size() > capacity_ - save_reports_.size())
                return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "save-all.results"});
            auto operation = sessions::SaveAllOperation::begin(sessions_, saves_);
            if (!operation)
                return failure("save-all", operation.error());
            save_all_ = std::move(*operation);
            save_all_tracked_ = 0;
            // The fixed set owns every accepted ID immediately, even if catalog lookup must retry.
            return {};
        }
'''+''.join('    '+line+'\n' if line else '\n' for line in common.splitlines())+'''    };
    ProjectContentSaving::ProjectContentSaving(
        sessions::SessionStore& sessions, sessions::SessionOpening& opening, persistence::SaveService& saves,
        ProjectStorage& project, persistence::WriteCoordinator& writes, persistence::IArtifactStore& files
    ) : impl_(std::make_unique<Impl>(sessions, opening, saves, project, writes, files))
    {}
    ProjectContentSaving::~ProjectContentSaving() = default;
'''
for ret,name,args,call in [
 ('PreparedProjectSave','prepare','sessions::ContentStamp source, persistence::ESaveMode mode, std::string destination','source, mode, std::move(destination)'),
 ('persistence::SaveId','request','sessions::ContentStamp source, persistence::ESaveMode mode, std::string destination','source, mode, std::move(destination)'),
 ('void','track','persistence::SaveId id, std::span<const ProjectAssetEntry> destinations','id, destinations'),
 ('void','update','std::span<const sessions::SaveAllEntry> borrowed','borrowed'),
 ('void','saveAll','','')]:
 source+=f'''    EditorResult<{ret}> ProjectContentSaving::{name}({args})
    {{
        if (auto admitted = impl_->admission(); !admitted)
            return cxx::unexpected(admitted.error());
        Impl::Dispatch scope{{impl_->dispatching_}};
        return impl_->{name}({call});
    }}
'''
source+='''    EditorResult<void> ProjectContentSaving::acknowledge(persistence::SaveId id)
    {
        if (auto admitted = impl_->admission(); !admitted)
            return admitted;
        Impl::Dispatch scope{impl_->dispatching_};
        auto found = std::ranges::find(impl_->save_reports_, id, &ProjectSaveReport::id);
        if (found == impl_->save_reports_.end())
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "save.result"});
        const bool is_pending = std::ranges::find(impl_->pending_saves_, id) != impl_->pending_saves_.end();
        if (!found->result || is_pending)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "save.result"});
        impl_->save_reports_.erase(found);
        return {};
    }
    EditorResult<void> ProjectContentSaving::acknowledgeSaveAll()
    {
        if (auto admitted = impl_->admission(); !admitted)
            return admitted;
        Impl::Dispatch scope{impl_->dispatching_};
        if (impl_->save_all_ && impl_->save_all_tracked_ != impl_->save_all_->entries().size())
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "save-all.association"});
        impl_->save_all_.reset();
        return {};
    }
    std::span<const ProjectSaveReport> ProjectContentSaving::reports() const noexcept { return impl_->save_reports_; }
    std::span<const persistence::SaveId> ProjectContentSaving::pending() const noexcept { return impl_->pending_saves_; }
    std::span<const sessions::SaveAllEntry> ProjectContentSaving::saveAllEntries() const noexcept
    {
        return impl_->save_all_ ? impl_->save_all_->entries() : std::span<const sessions::SaveAllEntry>{};
    }
    bool ProjectContentSaving::hasSaveAll() const noexcept { return impl_->save_all_.has_value(); }
    bool ProjectContentSaving::hasCapacity(std::size_t count) const noexcept
    {
        return count <= Impl::capacity_ - impl_->save_reports_.size();
    }
    bool ProjectContentSaving::settled() const noexcept
    {
        return impl_->pending_saves_.empty() &&
            (!impl_->save_all_ || impl_->save_all_tracked_ == impl_->save_all_->entries().size());
    }
}
'''
(root/'editor/activities/project/src/ProjectContentSaving.cpp').write_text(source)
