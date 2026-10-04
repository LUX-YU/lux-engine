from pathlib import Path
import re,json
r=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=r/'editor/application/src/EditorSaving.cpp'; s=p.read_text()
def cut(s,name):
 o=s.index('EditorApplication::Impl::'+name+'(')
 a=s.rfind('\n    EditorResult<',0,o)+1
 b=s.index('\n    }',o)+len('\n    }')
 return s[:a]+s[b:]
for name in ['prepareSave','rememberSave','settleSaves']: s=cut(s,name)
a=s.index('        auto prepared = prepareSave(target, mode, std::move(destination));'); b=s.index('\n    }',a)
s=s[:a]+'''        if (!target.based_on)
            return applicationFailure("save.source", sessions::ESessionError::STALE_CONTENT);
        return content_saving_->request(*target.based_on, mode, std::move(destination));'''+s[b:]
a=s.index('                auto operation = sessions::SaveAllOperation::begin'); b=s.index('                return commands::DispatchReceipt',a)
s=s[:a]+'''                auto operation = content_saving_->saveAll();
                if (!operation)
                    return cxx::unexpected(saveFailure(operation.error()));
'''+s[b:]
s=s.replace('save_reports_.size() + ids->size() > 128','!content_saving_->hasCapacity(ids->size())').replace('#include <random>\n','')
s=re.sub('\n{3,}','\n\n',s);p.write_text(s)
p=r/'editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp';s=p.read_text()
s=s.replace('#include <lux/engine/editor/storage/ArtifactPublicationOperation.hpp>','#include <lux/engine/editor/storage/ArtifactPublicationOperation.hpp>\n#include <lux/engine/editor/storage/ProjectContentSaving.hpp>')
for name in ['SavePresentation','PreparedSave']:
 a=s.index('        struct '+name+' final');b=s.index('\n        };',a)+len('\n        };');s=s[:a]+s[b:]
a=s.index('        [[nodiscard]] EditorResult<PreparedSave> prepareSave(');b=s.index(');',a)+2;s=s[:a]+s[b:]
s=s.replace('        [[nodiscard]] EditorResult<void> settleSaves();\n','').replace('        [[nodiscard]] EditorResult<void> rememberSave(persistence::SaveId);\n','')
s=s.replace('        std::vector<persistence::SaveId> pending_saves_;\n','').replace('        std::vector<SavePresentation> save_reports_;\n','').replace('        std::optional<sessions::SaveAllOperation> save_all_;\n','')
s=s.replace('        sessions::SessionOpening opening_;','        sessions::SessionOpening opening_;\n        std::unique_ptr<ProjectContentSaving> content_saving_;')
p.write_text(s)
p=r/'editor/application/src/EditorApplication.cpp';s=p.read_text().replace('        project_ = std::move(*project);','''        project_ = std::move(*project);
        content_saving_ = std::make_unique<ProjectContentSaving>(
            sessions_, opening_, saves_, *project_, writes_, files_
        );''');p.write_text(s)
p=r/'editor/application/src/EditorCommands.cpp';s=p.read_text();a=s.index('        auto result = impl_->commands_.execute');b=s.index('\n    }',a);s=s[:a]+'''        return impl_->commands_.execute(std::move(*handle), invocation);'''+s[b:];p.write_text(s)
p=r/'editor/application/src/EditorLifecycle.cpp';s=p.read_text()
s=s.replace('save_reports_.size() + ids->size() > 128','!content_saving_->hasCapacity(ids->size())')
s=s.replace('auto destination = prepareSave(\n                        {review_content_->session, *review_content_},','auto destination = content_saving_->prepare(\n                        *review_content_,')
s=re.sub(r'rememberSave\(\*(saved|entry|save)\.save\)',r'content_saving_->track(*\1.save, close_destinations_)',s)
# Registration now adds to the sole pending collection itself.
s=re.sub(r'\n\s+if \(std::ranges::find\(pending_saves_, \*(entry|save)\.save\) == pending_saves_\.end\(\)\)\n\s+pending_saves_\.push_back\(\*\1\.save\);','',s)
s=s.replace('                            pending_saves_.push_back(*saved.save);\n','')
# A trailing empty loop was only the former duplicate pending insertion.
s=re.sub(r'        for \(const auto& save : closing_->saves\(\)\)\n            if \(save.save\)\n            \{\n            \}\n','',s)
s=s.replace('receive(settleSaves());','receive(content_saving_->update(closing_ ? closing_->saves() : std::span<const sessions::SaveAllEntry>{}));')
a=s.index('                else if (const auto* admitted = std::get_if<commands::AcceptedOperation>');b=s.index('\n            }',a);s=s[:a]+s[b:]
s=s.replace('pending_saves_.empty() && opening_', 'content_saving_->settled() && opening_')
s=s.replace('pending_saves_','content_saving_->pending()').replace('save_reports_','content_saving_->reports()').replace('SavePresentation::','ProjectSaveReport::')
p.write_text(s)
p=r/'editor/application/src/EditorResults.cpp';s=p.read_text()
a=s.index('                std::erase_if(save_reports_');b=s.index('\n            }',a);s=s[:a]+'''                return content_saving_->acknowledge(action.target);'''+s[b:]
s=s.replace('                save_all_.reset(); // The accepted SaveIds remain in their original operation/report owners.','                return content_saving_->acknowledgeSaveAll();')
s=s.replace('save_reports_','content_saving_->reports()').replace('if (save_all_)','if (content_saving_->hasSaveAll())').replace('save_all_->entries()','content_saving_->saveAllEntries()');p.write_text(s)
p=r/'editor/application/src/EditorViewClosure.cpp';s=p.read_text().replace('save_reports_.size() >= 128','!content_saving_->hasCapacity(1)');p.write_text(s)
p=r/'editor/tests/integration/application/application.cpp';s=p.read_text().replace('impl.pending_saves_','impl.content_saving_->pending()').replace('impl.save_reports_','impl.content_saving_->reports()').replace('std::remove_reference_t<decltype(impl)>::SavePresentation::id','ProjectSaveReport::id');p.write_text(s)
p=r/'editor/activities/project/CMakeLists.txt';s=p.read_text().replace('src/ProjectStorage.cpp src/PreparedProjectOpen.cpp','src/ProjectStorage.cpp src/ProjectContentSaving.cpp src/PreparedProjectOpen.cpp');s=s.replace('target_compile_definitions(editor_storage PUBLIC', '''target_link_libraries(editor_storage PUBLIC lux::engine::editor::session_execution)
component_add_transitive_commands(editor_storage
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_execution)")
target_compile_definitions(editor_storage PUBLIC''',1);s=s.replace('    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectCreation.hpp','    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectContentSaving.hpp\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/ProjectCreation.hpp');p.write_text(s)
p=r/'editor/tests/architecture/rules.json';j=json.loads(p.read_text())
def update(v):
 if isinstance(v,dict):
  if 'editor/activities/project/include/lux/engine/editor/storage/ProjectCreation.hpp' in v:
   v['editor/activities/project/include/lux/engine/editor/storage/ProjectContentSaving.hpp']=['editor_storage']
   v['editor/activities/project/src/ProjectContentSaving.cpp']=['editor_storage']
  else:
   for a in v.values():update(a)
 elif isinstance(v,list):
  for a in v:update(a)
update(j);p.write_text(json.dumps(j,ensure_ascii=False,indent=2)+'\n')
