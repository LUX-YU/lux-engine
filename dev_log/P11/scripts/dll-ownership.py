from pathlib import Path
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
p=s/'editor/activities/persistence/src/SaveService.cpp';t=p.read_text();t=t.replace('        Impl(WriteCoordinator& value, SaveLimits policy) : coordinator(value), limits(policy) {}','''        // The service's creating module owns allocation/deallocation code for the weak registration
        // directory. A plugin may call prepareSource through its static SDK copy, then unload while
        // expired weak records remain; their control block must not have a plugin-local vtable.
        using AllocateSource = std::shared_ptr<SaveSourceRegistration::State>(*)(
            contracts::CodeLease, ISaveSource*, sessions::SessionId, const void*);
        AllocateSource allocate_source;
        Impl(WriteCoordinator& value, SaveLimits policy) : coordinator(value), limits(policy),
            allocate_source(+[](contracts::CodeLease code, ISaveSource* source, sessions::SessionId id,const void* service) {
                return std::make_shared<SaveSourceRegistration::State>(std::move(code),nullptr,source,id,service);
            }) {}''');t=t.replace('std::make_shared<SaveSourceRegistration::State>(std::move(code), nullptr, &source_value, id, this)','allocate_source(std::move(code), &source_value, id, this)');p.write_text(t)
# Attach the code lease outside the foreign shared_ptr, including its virtual control-block cleanup.
p=s/'editor/editing/include/lux/engine/editor/contracts/CodeLease.hpp';t=p.read_text();i=t.rfind('}');t=t[:i]+'''    // Call at the receiving module boundary. The returned alias owns the incoming control block,
    // then its defining code; even an expired weak alias can be destroyed after that code unloads.
    template<class T> [[nodiscard]] std::shared_ptr<T> pinCodeOwner(CodeLease code,std::shared_ptr<T> value)
    {
        struct Owner final { CodeLease code; std::shared_ptr<T> value; };
        auto owner=std::make_shared<Owner>(std::move(code),std::move(value));
        auto* pointer=owner->value.get();
        return std::shared_ptr<T>(std::move(owner),pointer);
    }
'''+t[i:];p.write_text(t)
for f,marker in [('editor/activities/commands/src/CommandRegistry.cpp','CommandRegistrySnapshot::create'),('editor/activities/sessions/src/SessionFactory.cpp','SessionFactorySnapshot::create'),('editor/workbench/desktop/src/ViewFactory.cpp','ViewFactorySnapshot::create')]:
 p=s/f;t=p.read_text();i=t.index('        for (std::size_t i{};',t.index(marker));t=t[:i]+'''        for (auto& entry : entries)
            if (entry) entry=contracts::pinCodeOwner(entry->code_,std::move(entry));
'''+t[i:];# copy code before moving shared_ptr: function argument evaluation would move first!
 t=t.replace('if (entry) entry=contracts::pinCodeOwner(entry->code_,std::move(entry));','if (entry) { auto code=entry->code_; entry=contracts::pinCodeOwner(std::move(code),std::move(entry)); }');p.write_text(t)
p=s/'editor/activities/sessions/src/SessionInstallation.cpp';t=p.read_text();
for name in ['InstalledSession','PreparedSessionInstallation']:
 t=t.replace(f'{name}::~{name}() = default;',f'''{name}::~{name}()
    {{
        const auto code=data_ ? data_->code : contracts::CodeLease::builtin();
        data_.reset();
    }}''');t=t.replace(f'{name}& {name}::operator=({name}&&) noexcept = default;',f'''{name}& {name}::operator=({name}&& other) noexcept
    {{
        if(this!=&other) {{ {name} previous(std::move(*this));data_=std::move(other.data_); }}
        return *this;
    }}''')
# close's pinned local needs external lease until the shared_ptr control block returns.
t=t.replace('        const auto pinned = data_;','        const auto code = data_ ? data_->code : contracts::CodeLease::builtin();\n        const auto pinned = data_;');p.write_text(t)
