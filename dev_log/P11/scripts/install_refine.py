from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-p11')
def edit(p,a,b):
 p=s/p;t=p.read_text();assert a in t,(p,a);p.write_text(t.replace(a,b),newline='\n')
p='editor/activities/persistence/src/SaveService.cpp'
edit(p,'        ISaveSource* source;','        std::unique_ptr<ISaveSource> owned_source;\n        ISaveSource* source;')
edit(p,'std::make_shared<SaveSourceRegistration::State>(std::move(code), &source_value, id, this)','std::make_shared<SaveSourceRegistration::State>(std::move(code), nullptr, &source_value, id, this)')
edit(p,'sessions::SessionId id, ISaveSource& source, contracts::CodeLease code\n    )','sessions::SessionId id, std::unique_ptr<ISaveSource> source, contracts::CodeLease code\n    )')
edit(p,'        if (!impl_->onOwner())\n            return failed(EPersistenceError::WRONG_THREAD);\n        if (impl_->dispatching)\n            return failed(EPersistenceError::BUSY);\n        const Impl::DispatchScope dispatch{impl_->dispatching};\n        return impl_->prepare(id, source, std::move(code));', '''        // External code pin encloses rejection cleanup as well as the entire source destructor.
        struct Input final { contracts::CodeLease code; std::unique_ptr<ISaveSource> source; };
        Input owned{std::move(code), std::move(source)};
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (impl_->dispatching)
            return failed(EPersistenceError::BUSY);
        const Impl::DispatchScope dispatch{impl_->dispatching};
        if (!owned.source)
            return failed(EPersistenceError::INVALID_ARGUMENT);
        auto prepared = impl_->prepare(id, *owned.source, owned.code);
        if (prepared)
            prepared->state_->owned_source = std::move(owned.source);
        return prepared;''')
edit('editor/activities/persistence/include/lux/engine/editor/persistence/SaveService.hpp','sessions::SessionId, ISaveSource&, contracts::CodeLease','sessions::SessionId, std::unique_ptr<ISaveSource>, contracts::CodeLease')
edit('editor/activities/persistence/include/lux/engine/editor/persistence/SaveSource.hpp','// The adapter must outlive any callback already on the stack; the token does not own it.','// registerSource borrows an adapter that must outlive active callbacks. prepareSource owns its adapter;\n    // revocation disables calls while active dispatch/operations retain the source and its external code pin.')
p='editor/activities/sessions/src/SessionInstallation.cpp'
edit(p,'std::unique_ptr<detail::SessionInstallationData> data','std::shared_ptr<detail::SessionInstallationData> data')
edit(p,'auto data = std::make_unique<detail::SessionInstallationData>','auto data = std::make_shared<detail::SessionInstallationData>')
edit(p,'saves.prepareSource(data->id, *data->source, data->code)','saves.prepareSource(data->id, std::move(data->source), data->code)')
edit(p,'    HistoryActions& InstalledSession::history() noexcept { return *data_->history; }','''    SessionFactoryResult<HistoryActionsInfo> InstalledSession::queryHistory() const
    {
        const auto pinned = data_;
        if (!pinned) return cxx::unexpected(factoryFailure(ESessionError::STALE_SESSION));
        return pinned->history->query();
    }
    SessionFactoryResult<ContentStamp> InstalledSession::undo()
    {
        const auto pinned = data_;
        if (!pinned) return cxx::unexpected(factoryFailure(ESessionError::STALE_SESSION));
        return pinned->history->undo();
    }
    SessionFactoryResult<ContentStamp> InstalledSession::redo()
    {
        const auto pinned = data_;
        if (!pinned) return cxx::unexpected(factoryFailure(ESessionError::STALE_SESSION));
        return pinned->history->redo();
    }''')
edit(p,'        auto permit = data_->store.prepareClose(expected);','        const auto pinned = data_;\n        auto permit = pinned->store.prepareClose(expected);')
edit(p,'auto closed = data_->store.close(*permit);','auto closed = pinned->store.close(*permit);')
p='editor/activities/sessions/include/lux/engine/editor/sessions/SessionInstallation.hpp'
edit(p,'std::unique_ptr<detail::SessionInstallationData>','std::shared_ptr<detail::SessionInstallationData>')
edit(p,'[[nodiscard]] HistoryActions& history() noexcept;', '''[[nodiscard]] SessionFactoryResult<HistoryActionsInfo> queryHistory() const;
        [[nodiscard]] SessionFactoryResult<ContentStamp> undo();
        [[nodiscard]] SessionFactoryResult<ContentStamp> redo();''')
p='editor/activities/sessions/sinclude/lux/engine/editor/detail/PrepareSession.hpp'
edit(p,'''            if (!view)
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::BUSY, "history", static_cast<std::uint64_t>(view.error().code)});''','''            if (!view)
            {
                auto code = ESessionFactoryError::CONSTRUCT;
                switch (view.error().code)
                {
                case editing::EEditError::BUSY: code = ESessionFactoryError::BUSY; break;
                case editing::EEditError::WRONG_THREAD: code = ESessionFactoryError::WRONG_THREAD; break;
                case editing::EEditError::CLOSED: code = ESessionFactoryError::CLOSED; break;
                default: break;
                }
                return cxx::unexpected(SessionFactoryFailure{code, "history", static_cast<std::uint64_t>(view.error().code)});
            }''')
p='editor/tests/integration/session_factories/installation.cpp'
edit(p,'entry.history().query()','entry.queryHistory()');edit(p,'entry.history().undo()','entry.undo()');edit(p,'entry.history().redo()','entry.redo()')
edit(p,'vfs.mount({"",','vfs.mount({"sources",')
f=s/'editor/tests/architecture/rules.json';r=json.loads(f.read_text());r['editor_layering']['targets']['editor_session_installation_test']={'layer':'TEST','role':'TEST','capabilities':['CPU','PROCESS']};f.write_text(json.dumps(r,indent=2)+'\n')
