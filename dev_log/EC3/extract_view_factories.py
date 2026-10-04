from pathlib import Path
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=s/'editor/application/extensions/src/BuiltinContributions.cpp';a=p.read_text()
# Four production factories move, unchanged responsibility, to actual tools. No forwarding functions.
def function(text,name):
 start=text.rfind('    std::shared_ptr<views::ViewFactoryEntry>',0,text.index(name+'('))
 body=text.index('{',text.index(name+'(',start));depth=1;i=body+1
 while depth:
  depth += (text[i]=='{')-(text[i]=='}');i+=1
 return start,i,text[start:i]
helper_start=a.index('        template <class Error>')
helper_end=a.index('        template <class Input, class Create>')
helpers=a[helper_start:helper_end]
helper='''#pragma once
#include <lux/engine/editor/views/ViewFactory.hpp>
#include <algorithm>

namespace lux::editor::workbench::detail
{
'''+''.join(line[4:]+'\n' for line in helpers.splitlines())+'''    template <const views::ViewFactoryDescriptor& Descriptor, class Input, class Create>
        requires requires(Create& create, const views::ViewFactoryInput& input, const Input& value) {
            create(input, value);
        }
    std::shared_ptr<views::ViewFactoryEntry> bindViewFactory(Create create)
    {
        static_assert(Descriptor.binding_type == cxx::typeToken<Input>());
        return views::ViewFactoryEntry::bind<Descriptor>(
            contracts::CodeLease::builtin(),
            [create = std::move(create)](const views::ViewFactoryInput& input) mutable
                -> views::ViewFactoryResult<views::DetachedView> {
                auto view = create(input, *static_cast<const Input*>(input.binding()));
                if (!view)
                    return cxx::unexpected(viewFailure(view.error()));
                return std::move(*view);
            }
        );
    }
}
'''
out=s/'editor/workbench/sinclude/lux/engine/editor/workbench/ViewFactorySupport.hpp';out.write_text(helper)
for file,ns,old,new,decl,head in [
 ('scene/SceneView','scene','builtinSceneViewFactory','makeSceneViewFactory','lux.editor.scene.view','Scene'),
 ('material/MaterialView','material','builtinMaterialViewFactory','makeMaterialViewFactory','lux.editor.material','Material'),
 ('flow/FlowView','flowforge','builtinFlowViewFactory','makeFlowViewFactory','lux.editor.flowforge','FlowForge')]:
 start,end,body=function(a,old);a=a[:start]+a[end:]
 body=body.replace(old,new).replace('ModelIntent','cxx::move_only_function<void(const ModelPlacement&)>').replace('ArtifactIntent','cxx::move_only_function<void(const persistence::DerivedArtifact&)>')
 kind={'scene':'lux.editor.scene','material':'lux.editor.material','flowforge':'lux.editor.flowforge'}[ns]
 # Remove passed metadata; the module references its single fixed declaration.
 import re
 body=re.sub(r'return viewFactory<views::ContentViewInput>\(\s*views::ViewTypeId\{"[^"]+"\}, "[^"]+", \{"[^"]+"\},',
             'return workbench::detail::bindViewFactory<kViewDescriptor, views::ContentViewInput>(',body)
 body=body.replace('viewFailure(', 'workbench::detail::viewFailure(').replace('connectIntent(', 'workbench::detail::connectIntent(')
 constants=f'''    namespace
    {{
        constexpr sessions::SessionKindIdView kContentKinds[]{{sessions::SessionKindIdView{{"{kind}"}}}};
        constexpr views::ViewFactoryDescriptor kViewDescriptor{{
            views::ViewTypeIdView{{"{decl}"}}, "{head}", cxx::typeToken<views::ContentViewInput>(), 1, kContentKinds
        }};
    }}
'''
 folder,stem=file.split('/');cpp=s/f'editor/workbench/{folder}/src/{stem}.cpp';c=cpp.read_text();c='#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>\n'+c;c+=f'\nnamespace lux::editor::{ns}\n{{\n'+constants+body+'\n}\n';cpp.write_text(c)
 h=s/f'editor/workbench/{folder}/include/lux/engine/editor/{ns}/{stem}.hpp';text=h.read_text();text=text.replace('namespace lux::editor::'+ns+'\n{','namespace lux::editor::views { class ViewFactoryEntry; }\n\nnamespace lux::editor::'+ns+'\n{',1)
 sig=body[:body.index('\n    {')].replace('    std::shared_ptr','    [[nodiscard]] std::shared_ptr')
 sig=sig.replace('cxx::move_only_function<void(const ModelPlacement&)> receiver','cxx::move_only_function<void(const ModelPlacement&)> = {}').replace('cxx::move_only_function<void(const persistence::DerivedArtifact&)> receiver','cxx::move_only_function<void(const persistence::DerivedArtifact&)> = {}')
 last=text.rfind('}');text=text[:last]+sig+';\n'+text[last:];h.write_text(text)
# Remaining helper used by SceneCreation only, retained until its exact migration.
p.write_text(a)
h=s/'editor/application/extensions/include/lux/engine/editor/extensions/BuiltinContributions.hpp';text=h.read_text();start=text.index('    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinSceneViewFactory');text=text[:start]+'}\n';text=text.replace('    using ArtifactIntent = cxx::move_only_function<void(const persistence::DerivedArtifact&)>;\n','').replace('    using ModelIntent = cxx::move_only_function<void(const scene::ModelPlacement&)>;\n','');h.write_text(text)
p=s/'editor/application/src/EditorCommands.cpp';text=p.read_text().replace('extensions::builtinSceneViewFactory','scene::makeSceneViewFactory').replace('extensions::builtinMaterialViewFactory','material::makeMaterialViewFactory').replace('extensions::builtinFlowViewFactory','flowforge::makeFlowViewFactory');p.write_text(text)
# Public signatures forward declare the factory; implementation consumes the precise private provider.
for folder,target in [('material','material_ui'),('flow','flow_ui')]:
 p=s/f'editor/workbench/{folder}/CMakeLists.txt';text=p.read_text().replace('PRIVATE imgui::core)','PRIVATE lux::engine::editor::view_host imgui::core)').replace('    "find_package(imgui REQUIRED COMPONENTS core)")','    "find_package(lux-engine-editor-desktop REQUIRED COMPONENTS view_host)"\n    "find_package(imgui REQUIRED COMPONENTS core)")');p.write_text(text)
# No remaining use of the old generic metadata helper.
p=s/'editor/application/extensions/src/BuiltinContributions.cpp';text=p.read_text();start=text.index('        template <class Input, class Create>');end=text.index('    std::vector<std::shared_ptr<commands::CommandEntry>>',start);text=text[:start]+'    }\n'+text[end:];p.write_text(text)
