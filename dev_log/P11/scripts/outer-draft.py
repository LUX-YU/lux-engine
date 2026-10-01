from pathlib import Path
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
for f,marker in [('editor/activities/commands/src/CommandRegistry.cpp','CommandRegistrySnapshot::create'),('editor/activities/sessions/src/SessionFactory.cpp','SessionFactorySnapshot::create'),('editor/workbench/desktop/src/ViewFactory.cpp','ViewFactorySnapshot::create')]:
 p=s/f;t=p.read_text();line='        for (auto& entry : entries)\n            if (entry) { auto code=entry->code_; entry=contracts::pinCodeOwner(std::move(code),std::move(entry)); }\n';t=t.replace(line,'');i=t.index('        if (entries.size() > capacity)',t.index(marker));t=t[:i]+line+t[i:];p.write_text(t)
p=s/'editor/application/extensions/include/lux/engine/editor/extensions/Contributions.hpp';t=p.read_text();t=t.replace('    struct ContributionDraft final\n    {','''    struct ContributionDraft final
    {
        ContributionDraft()=default;
        ContributionDraft(ContributionDraft&&) noexcept=default;
        ContributionDraft& operator=(ContributionDraft&&)=delete;
        ContributionDraft(const ContributionDraft&)=delete;
        ContributionDraft& operator=(const ContributionDraft&)=delete;
        // External draft-level pin also covers rejected entries before catalog normalization.
        std::vector<contracts::CodeLease> code;''');p.write_text(t)
p=s/'editor/application/extensions/src/Contributions.cpp';t=p.read_text().replace('        std::shared_ptr<const void> reflection_lifetime;','        std::vector<contracts::CodeLease> code;\n        std::shared_ptr<const void> reflection_lifetime;').replace('std::make_shared<Data>(std::move(lifetime),','std::make_shared<Data>(std::move(draft.code), std::move(lifetime),');p.write_text(t)
p=s/'editor/application/extensions/src/EditorExtension.cpp';t=p.read_text().replace('        const auto lease = contracts::CodeLease::plugin(pinned);','        const auto lease = contracts::CodeLease::plugin(pinned);\n        draft.code.push_back(lease);');p.write_text(t)
