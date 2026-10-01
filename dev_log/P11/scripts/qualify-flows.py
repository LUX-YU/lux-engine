from pathlib import Path
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
p=s/'cmake/installed-consumers/editor-p11/main.cpp';t=p.read_text();needle='        auto snapshot = take(extensions::ContributionSnapshot::prepare(take(extension.contributions())));';t=t.replace(needle,'''        auto bad_draft=take(extension.contributions());
        bad_draft.configurations[0].value.reflection=[](meta::ReflectionRegistry&) noexcept -> const meta::RefClass* { return nullptr; };
        auto bad=take(extensions::ContributionSnapshot::prepare(std::move(bad_draft)));
        assert(catalog.enqueue(bad));
        const auto rejected=catalog.applyPending();
        assert(!rejected && rejected.error().domain=="configuration.reflection" && catalog.revision()==0);
        assert(!catalog.snapshot().valid());
'''+needle);p.write_text(t)
p=s/'editor/tests/integration/session_factories/installation.cpp';t=p.read_text();needle='        root.menu=nullptr;';t=t.replace(needle,'''        // A queued command keeps its original identity after closure and actual slot reuse.
        auto factory=take(factories.find(kinds[1]));
        SessionLoadInput input{vfs.view().capture(),identity(SourceFiles::names[1]),
            BoundSource{identity(SourceFiles::names[1]),SourceFiles::names[1]},take(disk.resolve(SourceFiles::names[1]))};
        auto open_extra=[&]() {
            auto decoded=take(load(runtime,factory,input));auto prepared=take(std::move(decoded).prepare(store,saves));
            return take(prepared.publish());
        };
        installed.push_back(open_extra());selected=installed.back().id();
        const auto abandoned_id=selected;
        CommandInvocation abandoned{SessionTarget{selected}};
        assert(dispatcher.enqueue(take(snapshot.find(CommandIdView{"lux.editor.save"})),abandoned));
        assert(installed.back().close(take(store.describe(selected)).current));installed.pop_back();
        auto replacement=open_extra();const auto replacement_before=take(store.describe(replacement.id()));
        assert(replacement.id()!=abandoned_id);
        assert(menu.update());completed=menu.takeCompletions();
        assert(completed.size()==1 && !completed[0].result && completed[0].result.error().code==ECommandError::STALE_TARGET);
        assert(take(store.describe(replacement.id())).current==replacement_before.current);
        assert(replacement.close(replacement_before.current));
        root.menu=nullptr;''');needle='    for(auto& entry:installed)\n    {\n        auto info=';t=t.replace(needle,'''    // All three factory-installed roles use the existing Save As/rebind and Export Copy algorithms.
    for(std::size_t index{};index<installed.size();++index)
    {
        auto& entry=installed[index];
        const auto original=take(store.describe(entry.id()));
        const auto copy_path="copy-"+std::to_string(index);
        const auto new_path="renamed-"+std::to_string(index);
        const auto copy=take(saves.requestSave({entry.id(),ESaveMode::EXPORT_COPY,take(disk.resolve(copy_path)),identity(copy_path)}));
        const auto drain_save=[&](SaveId id) {
            for(unsigned turn{};turn<10000;++turn)
            {
                assert(execution.submitReady() && runtime.collectCompletions());saves.adoptCompletions();
                if(take(saves.status(id)).stage==ESaveStage::TERMINAL) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            auto result=take(saves.status(id));assert(result.outcome);assert(saves.acknowledge(id));return *result.outcome;
        };
        drain_save(copy);
        const auto after_copy=take(store.describe(entry.id()));
        assert(after_copy.current==original.current && after_copy.binding==original.binding && after_copy.dirty==original.dirty);
        assert(std::filesystem::exists(root/copy_path));
        const auto as=take(saves.requestSave({entry.id(),ESaveMode::SAVE_AS,take(disk.resolve(new_path)),identity(new_path)}));
        assert(drain_save(as).adoption==EAdoption::APPLIED);
        const auto after_as=take(store.describe(entry.id()));
        assert(after_as.current==original.current && !after_as.dirty && after_as.binding!=original.binding);
        assert(take(entry.queryHistory()).can_undo && entry.undo() && entry.redo());
        assert(take(store.describe(entry.id())).current==original.current && std::filesystem::exists(root/new_path));
    }
    for(auto& entry:installed)
    {
        auto info=''');p.write_text(t)
