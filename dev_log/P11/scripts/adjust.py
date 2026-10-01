from pathlib import Path
import re,json
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
for name in ['main.cpp','Extension.cpp']:
 p=s/'cmake/installed-consumers/editor-p11'/name;t=p.read_text();t=re.sub(r'(?<![:\w])material::','lux::editor::material::',t);t=re.sub(r'(?<![:\w])project::','lux::project::',t);p.write_text(t)
p=s/'editor/application/extensions/include/lux/engine/editor/extensions/Contributions.hpp';t=p.read_text();t=t.replace('    // Mutable preparation only.', '''    struct ReflectionContribution final
    {
        contracts::CodeLease code;
        meta::ReflectionRegistrationDraft::RegisterFn register_types{};
    };
    // Mutable preparation only.''');t=t.replace('        std::vector<std::shared_ptr<commands::CommandEntry>> commands;','        std::vector<ReflectionContribution> reflection;\n        std::vector<std::shared_ptr<commands::CommandEntry>> commands;');t=t.replace('        struct Data;\n','        friend class ContributionRegistry;\n        struct Data;\n');p.write_text(t)
p=s/'editor/application/extensions/src/Contributions.cpp';t=p.read_text();t=t.replace('#include <deque>','#include <lux/engine/editor/configuration/EditorReflection.hpp>\n#include <deque>');t=t.replace('        commands::CommandRegistrySnapshot commands;','        std::shared_ptr<const void> reflection_lifetime;\n        std::vector<ReflectionContribution> reflection;\n        commands::CommandRegistrySnapshot commands;');t=t.replace('if (draft.configurations.size() > capacity || draft.components.size() > capacity)','if (draft.configurations.size() > capacity || draft.components.size() > capacity || draft.reflection.size() > capacity)');needle='        ContributionSnapshot result;';t=t.replace(needle,'''        for (const auto& entry : draft.reflection)
            if (!entry.code.valid() || !entry.register_types)
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "reflection"});
        auto lifetime = draft.reflection.empty() && draft.configurations.empty() ?
            std::shared_ptr<const void>{} : acquireEditorReflection();
        ContributionSnapshot result;''');t=t.replace('std::make_shared<Data>(std::move(*commands)', 'std::make_shared<Data>(std::move(lifetime), std::move(draft.reflection), std::move(*commands)');needle='            // Prepared, non-allocating swaps.';t=t.replace(needle,'''            // Foreign reflection callbacks prepare only the original registry's isolated draft. They
            // may queue a later contribution, but cannot publish during this batch. Failure discards
            // the whole candidate and leaves every live catalog unchanged.
            if (candidate.data_->reflection_lifetime)
            {
                auto reflection = meta::ReflectionRegistry::beginDraft();
                for (const auto& entry : candidate.data_->reflection)
                {
                    auto appended = reflection.appendOnce(entry.register_types,
                        std::make_shared<contracts::CodeLease>(entry.code));
                    if (!appended) return cxx::unexpected(ContributionFailure{EContributionError::CALLBACK,
                        "reflection.register", static_cast<std::uint64_t>(appended.error().code)});
                }
                for (const auto& entry : candidate.data_->configurations)
                {
                    const auto* type = entry.value.reflection(*reflection.registry());
                    const bool mismatch = !type || type->type.ptr != type ||
                        type->type.hash != entry.value.codec.type.hash() || type->type.name != entry.value.codec.type.name();
                    if (mismatch) return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT,
                        "configuration.reflection", 0, entry.value.schema_name});
                }
                auto committed = reflection.commit();
                if (!committed) return cxx::unexpected(ContributionFailure{EContributionError::CALLBACK,
                    "reflection.commit", static_cast<std::uint64_t>(committed.error().code)});
            }
            // Prepared, non-allocating swaps.''');p.write_text(t)
p=s/'editor/application/extensions/include/lux/engine/editor/extensions/EditorExtension.hpp';t=p.read_text().replace('components{};','components{}, reflection{};');p.write_text(t)
p=s/'editor/application/extensions/src/EditorExtension.cpp';t=p.read_text().replace('counts.configurations > 256','counts.reflection > 256 || counts.configurations > 256');t=t.replace('counts.views != draft.views.size()', 'counts.reflection != draft.reflection.size() || counts.views != draft.views.size()');t=t.replace('        for (const auto& entry : draft.commands)', '''        for (const auto& entry : draft.reflection)
            if (!entry.code.sameOwner(lease))
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "reflection.code"});
        for (const auto& entry : draft.commands)''');p.write_text(t)
# A configuration value must retain reflection storage independently from its creating registry/control.
p=s/'editor/authoring/configuration/src/ConfigurationValue.cpp';t=p.read_text().replace('#include <lux/engine/editor/configuration/ConfigurationValue.hpp>','#include <lux/engine/editor/configuration/ConfigurationValue.hpp>\n#include <lux/engine/editor/configuration/EditorReflection.hpp>');t=t.replace('        return ConfigurationValue(std::move(code),', '''        struct Lifetime final { std::shared_ptr<const void> reflection, code; };
        auto lifetime = std::make_shared<Lifetime>(acquireEditorReflection(), std::move(code));
        return ConfigurationValue(std::move(lifetime),''');p.write_text(t)
p=s/'editor/tests/architecture/rules.json';d=json.loads(p.read_text());
for k,v in d['editor_layering']['shared_headers'].items():
 if 'PrepareSession' in k:v['layer']='E2'
p.write_text(json.dumps(d,indent=2)+'\n')
