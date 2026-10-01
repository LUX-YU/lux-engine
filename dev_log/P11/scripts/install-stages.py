from pathlib import Path
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
p=s/'editor/tests/integration/session_factories/installation.cpp';t=p.read_text();i=t.index('    SessionFactoryResult<PreparedSessionData> load(');t=t[:i]+'''    // Real material roles exercise each public preparation boundary; no alternate author/history model.
    class MaterialHistory final : public HistoryActions
    {
    public:
        MaterialHistory(TSessionAccess<em::MaterialSession> access,TSessionKey<em::MaterialSession> key)
            : access_(access),key_(key) {}
        SessionFactoryResult<HistoryActionsInfo> query() const override
        {
            auto model=access_.read(key_);if(!model) return cxx::unexpected(factoryFailure(model.error()));
            auto history=take(model->get().historyView());
            return HistoryActionsInfo{{key_.id(),history.snapshot.current},history.can_undo,history.can_redo};
        }
        SessionFactoryResult<ContentStamp> undo() override
        { auto& model=take(access_.edit(key_)).get();assert(model.undo());return model.describe().current; }
        SessionFactoryResult<ContentStamp> redo() override
        { auto& model=take(access_.edit(key_)).get();assert(model.redo());return model.describe().current; }
    private:
        TSessionAccess<em::MaterialSession> access_;TSessionKey<em::MaterialSession> key_;
    };
    void installationStages(std::span<const std::byte> bytes)
    {
        SessionStore store{1};WriteCoordinator writes;SaveService saves{writes};
        for(unsigned boundary{};boundary!=8;++boundary)
        {
            SessionId id;unsigned released{};
            const auto exercise=[&] {
                auto owner=std::shared_ptr<const void>(new int{},[&](const void* p) {++released;delete static_cast<const int*>(p);});
                auto code=contracts::CodeLease::plugin(owner);
                auto decoded=take(em::MaterialCodec::decode(bytes));
                if(boundary==0) return;
                auto reservation=take(store.reserve<em::MaterialSession>({"lux.editor.material"},code));id=reservation.id();
                assert(!store.describe(id) && !saves.requestSave({id}));if(boundary==1) return;
                auto model=take(std::move(decoded).createSession(id,{},code));
                assert(model->historyView());if(boundary==2) return;
                assert(store.prepare(reservation,model));assert(!model && !store.describe(id));if(boundary==3) return;
                auto key=take(store.key<em::MaterialSession>(reservation));
                auto history=std::make_unique<MaterialHistory>(store.access<em::MaterialSession>(),key);
                assert(!history->query());if(boundary==4) return;
                auto source=std::make_unique<em::MaterialSaveSource>(store.access<em::MaterialSession>(),key,std::nullopt,BindingRevision{1});
                assert(!source->describe());if(boundary==5) return;
                auto prepared=take(PreparedSessionInstallation::prepare(store,saves,std::move(reservation),code,std::move(history),std::move(source)));
                assert(!store.describe(id) && !saves.requestSave({id}));if(boundary==6) return;
                auto installed=take(prepared.publish());assert(store.size()==1 && store.describe(id));
                auto messages=take(object::ObjectMessageQueue::create(4));
                struct Notice final : object::LuxObject { using LuxObject::LuxObject;object::TSignal<> installed{*this}; } notice{messages.dispatcherRef()};
                object::LuxObject receiver{messages.dispatcherRef()};
                auto connection=take(object::LuxObject::connect(&notice,&Notice::installed,&receiver,[]() noexcept {std::abort();},object::EDelivery::QUEUED));
                messages.close();const auto notification=notice.emit(notice.installed);
                assert(notification.closed==1 && store.describe(id) && installed.queryHistory());
                assert(installed.close(take(store.describe(id)).current));
            };
            exercise();assert(released==1 && store.size()==0 && !store.describe(id) && !saves.requestSave({id}));
            assert(store.canReserve());
        }
        std::cout<<"PASS eight real decode/installation/role/publication boundaries and closed notification\\n";
    }
'''+t[i:];t=t.replace('    write(root / SourceFiles::names[1], std::as_bytes(std::span{material_bytes}));','    write(root / SourceFiles::names[1], std::as_bytes(std::span{material_bytes}));\n    installationStages(std::as_bytes(std::span{material_bytes}));');needle='        auto decoded=take(load(runtime, factory, input));';t=t.replace(needle,'''        auto missing=input;missing.asset=identity("absent");assert(!load(runtime,factory,missing));
        auto oversized=input;oversized.max_bytes=1;assert(!load(runtime,factory,oversized));
        const auto original_bytes=read(root/SourceFiles::names[i]);
        write(root/SourceFiles::names[i],std::span<const std::byte>{});assert(!load(runtime,factory,input));
        write(root/SourceFiles::names[i],original_bytes);
'''+needle,1);p.write_text(t)
