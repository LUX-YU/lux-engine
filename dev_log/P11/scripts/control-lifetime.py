from pathlib import Path
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
for header,source,name in [('editor/activities/commands/include/lux/engine/editor/commands/Command.hpp','editor/activities/commands/src/CommandRegistry.cpp','CommandArguments'),('editor/workbench/desktop/include/lux/engine/editor/views/ViewFactory.hpp','editor/workbench/desktop/src/ViewFactory.cpp','ViewFactoryInput')]:
 p=s/header;t=p.read_text();t=t.replace(f'        {name}& operator=(const {name}&) noexcept = default;\n','');t=t.replace(f'        {name}& operator=({name}&&) noexcept = default;',f'        {name}& operator=({name}) noexcept;');p.write_text(t)
 p=s/source;t=p.read_text().replace(f'    {name}::~{name}() = default;',f'''    {name}::~{name}()
    {{
        const auto code = data_ ? data_->code : contracts::CodeLease::builtin();
        data_.reset();
    }}
    {name}& {name}::operator=({name} other) noexcept
    {{
        data_.swap(other.data_);
        return *this;
    }}''');p.write_text(t)
p=s/'cmake/installed-consumers/editor-p11/main.cpp';t=p.read_text();t=t.replace('    std::weak_ptr<const void> weak_library;','    std::weak_ptr<const void> weak_library;\n    std::weak_ptr<commands::CommandEntry> retired_entry;');t=t.replace('        const auto current = catalog.snapshot();','        const auto current = catalog.snapshot();\n        retired_entry=current.commands().entries().front();');t=t.replace('    assert(weak_library.expired() && facts.unloaded==1);','    assert(weak_library.expired() && facts.unloaded==1);\n    assert(retired_entry.expired());retired_entry.reset(); // Its receiver-owned control block is independent of the unloaded DLL.');p.write_text(t)
