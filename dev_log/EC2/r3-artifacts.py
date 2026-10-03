from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=s/'editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp'
t=p.read_text();t=t.replace('#pragma once','#pragma once\n#include <lux/engine/editor/storage/ArtifactPublicationOperation.hpp>',1)
a=t.index('        struct CompiledPackage final');b=t.index('        struct SavePresentation final',a)
t=t[:a]+'''        struct ArtifactPresentation final
        {
            std::uint64_t id;
            std::optional<persistence::DerivedArtifact> pending;
            std::unique_ptr<ArtifactPublicationOperation> operation;
            std::optional<EditorFailure> failure;
            [[nodiscard]] bool terminal() const noexcept
            {
                return failure.has_value() || (operation && operation->terminal());
            }
        };
'''+t[b:]
a=t.index('        struct CancelSave final')
t=t[:a]+'''        struct RetryArtifact final { std::uint64_t target; };
        struct AbandonArtifact final { std::uint64_t target; };
'''+t[a:]
t=t.replace('            AcknowledgeArtifact,','            AcknowledgeArtifact,\n            RetryArtifact,\n            AbandonArtifact,')
p.write_text(t,newline='\n')
p=s/'editor/application/src/EditorArtifacts.cpp';t=p.read_text();a=t.index('    EditorResult<void> EditorApplication::Impl::settleArtifacts()')
t=t[:a]+'''    EditorResult<void> EditorApplication::Impl::settleArtifacts()
    {
        for (auto& entry : artifacts_)
        {
            if (entry.terminal())
                continue;
            if (entry.pending)
            {
                if (phase_ != EApplicationPhase::RUNNING)
                {
                    entry.failure = EditorFailure{EEditorError::CLOSING, "artifact.admission"};
                    continue;
                }
                auto accepted = ArtifactPublicationOperation::create(*entry.pending, sessions_, *project_,
                    engine_->execution(), writes_, files_, save_execution_);
                if (!accepted)
                {
                    if (accepted.error().code != EEditorError::BUSY)
                        entry.failure = accepted.error();
                    continue;
                }
                entry.operation = std::move(*accepted);
                entry.pending.reset();
            }
            entry.operation->update();
        }
        return {};
    }
}
'''
t=t.replace('#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>\n','').replace('#include <random>\n','').replace('#include <algorithm>\n','')
p.write_text(t,newline='\n')
p=s/'editor/application/src/EditorLifecycle.cpp';t=p.read_text().replace('artifact.settled','artifact.terminal()').replace('value.settled','value.terminal()').replace('artifact.catalog_ticket ? artifact.catalog_ticket : artifact.ticket','artifact.operation ? artifact.operation->ticket() : std::nullopt');p.write_text(t,newline='\n')
p=s/'editor/application/src/EditorResults.cpp';t=p.read_text().replace('entry.settled','entry.terminal()')
a=t.index('            else if constexpr (std::same_as<Action, AcknowledgeSave>)')
t=t[:a]+'''            else if constexpr (std::same_as<Action, RetryArtifact> || std::same_as<Action, AbandonArtifact>)
            {
                const auto found = std::ranges::find(artifacts_, action.target, &ArtifactPresentation::id);
                if (found == artifacts_.end() || !found->operation)
                    return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "artifact.result"});
                if constexpr (std::same_as<Action, RetryArtifact>)
                    return found->operation->retry();
                else
                    found->operation->abandon();
            }
'''+t[a:]
a=t.index('                        ImGui::TextWrapped("%s", report.asset.cooked_path.c_str());');b=t.index('                        ImGui::PopID();',a)
t=t[:a]+'''                        if (report.failure)
                            ImGui::TextWrapped("%s: %s", report.failure->domain.c_str(), report.failure->message.c_str());
                        if (report.operation)
                        {
                            ImGui::TextWrapped("%s", std::string(report.operation->path()).c_str());
                            const auto& status = report.operation->status();
                            if (const auto* failed = std::get_if<EditorFailure>(&status))
                            {
                                ImGui::TextWrapped("%s: %s", failed->domain.c_str(), failed->message.c_str());
                                button("Retry retained publication", RetryArtifact{report.id});
                                button("Abandon remaining publication", AbandonArtifact{report.id});
                            }
                            else if (std::holds_alternative<PublicationSucceeded>(status))
                                ImGui::TextUnformatted("Package and catalog published. Author save baseline is unchanged.");
                            else if (const auto* abandoned = std::get_if<PublicationAbandoned>(&status))
                                ImGui::Text("Publication stopped; %zu files already published remain on disk.",
                                    abandoned->published_files);
                        }
                        if (report.terminal())
                            button("Acknowledge publication", AcknowledgeArtifact{report.id});
'''+t[b:]
p.write_text(t,newline='\n')
p=s/'editor/activities/project/CMakeLists.txt';t=p.read_text().replace('src/ProjectPublicationOperation.cpp)','src/ProjectPublicationOperation.cpp src/ArtifactPublicationOperation.cpp)',1).replace('    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectCreation.hpp','    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ArtifactPublicationOperation.hpp\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectCreation.hpp',1);p.write_text(t,newline='\n')
p=s/'editor/tests/architecture/rules.json';j=json.loads(p.read_text());
for path in ['include/lux/engine/editor/storage/ArtifactPublicationOperation.hpp','src/ArtifactPublicationOperation.cpp']:
 j['editor_layering']['files']['editor/activities/project/'+path]=['editor_storage']
p.write_text(json.dumps(j,indent=2)+'\n',newline='\n')
l=Path('E:/SyncForder/CodeRepos/lux-engine/.internal/editor-redesign/migration-ledger.json');j=json.loads(l.read_text(encoding='utf8'));j['ec2']['status']='R3_IN_PROGRESS';j['ec2']['batches']['R2']={'status':'IMPLEMENTED_DEVELOPMENT_VALIDATED','sha':'28c70bf74342cf92b33fd8cefff370bdaac275c4','evidence':['r2-source-order-build','r2-opaque-build','r2-source-preservation'],'note':'22 actual tests passed. Initial failures preserved including zero Transform defaults and UI feature reorder. Final clean-SHA SDK matrix pending.'};j['ec2']['batches']['R3']='IN_PROGRESS';l.write_text(json.dumps(j,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
