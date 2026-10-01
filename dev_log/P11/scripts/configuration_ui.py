from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-p11')
def edit(p,a,b):
 p=s/p;t=p.read_text();assert a in t,(p,a);p.write_text(t.replace(a,b),newline='\n')
old=s/'editor/plugins/src/ConfigurationForm.hpp';t=old.read_text().replace('#include <lux/engine/editor/configuration/ConfigurationValue.hpp>','#include <lux/engine/editor/scene/ConfigurationEditor.hpp>')
t=t.replace('template <class Configuration> ConfigurationEditorRegistration configurationEditor(const char* schema)','template <class Configuration> scene::ConfigurationEditor configurationEditor(const char* schema)')
t=t.replace('''        return {
            schema,
            1,''','''        return {
            contracts::CodeLease::builtin(),
            {schema,
            1,''')
t=t.replace('''                return registry.findClass(lux::cxx::typeToken<Configuration>().name());
            },''','''                return registry.findClass(lux::cxx::typeToken<Configuration>().name());
            }},''')
new=s/'editor/workbench/scene/include/lux/engine/editor/scene/ConfigurationForm.hpp';new.write_text(t,newline='\n');old.unlink()
p='editor/plugins/src/RenderFeatureEditorExports.cpp'
edit(p,'#include "ConfigurationForm.hpp"','#include <lux/engine/editor/scene/ConfigurationForm.hpp>')
edit(p,'extern "C" LUX_RENDER_FEATURE_META_PUBLIC', '''// P12: only the old V6 export table consumes this field-layout conversion; the form has one implementation.
namespace
{
    template<class T> lux::editor::ConfigurationEditorRegistration legacyConfigurationEditor(const char* schema)
    {
        const auto entry = lux::editor::detail::configurationEditor<T>(schema);
        return {schema, entry.value.schema_version, entry.value.codec, entry.value.reflection, entry.create, {}};
    }
}
extern "C" LUX_RENDER_FEATURE_META_PUBLIC''')
edit(p,'lux::editor::detail::configurationEditor<::','legacyConfigurationEditor<::')
# The retained DLL instantiates the same installed form; it does not link a formal provider back to metadata.
edit('editor/plugins/CMakeLists.txt','target_link_libraries(render_feature_meta PRIVATE','target_link_libraries(render_feature_meta PRIVATE lux::engine::editor::scene_ui')
# The temporary legacy DLL -> scene_ui edge is finite; no formal module links the legacy DLL.
p='editor/workbench/scene/CMakeLists.txt'
edit(p,'SOURCE_FILES src/SceneCreationPoint.cpp','SOURCE_FILES src/ConfigurationEditor.cpp src/SceneCreationPoint.cpp')
edit(p,'target_link_libraries(scene_ui PUBLIC','target_link_libraries(scene_ui PUBLIC lux::engine::editor::editor_configuration')
edit(p,'component_add_transitive_commands(scene_ui','component_add_transitive_commands(scene_ui\n    "find_package(lux-engine-editor-configuration REQUIRED COMPONENTS editor_configuration)"')
p='editor/workbench/desktop/CMakeLists.txt'
edit(p,'SOURCE_FILES src/ViewHost.cpp','SOURCE_FILES src/ViewHost.cpp src/ViewFactory.cpp')
edit(p,'    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/DesktopError.hpp','    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/views/ViewFactory.hpp\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/DesktopError.hpp')
# ViewFactory has a views logical directory, so install it there instead of the desktop directory.
edit(p,'    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/views/ViewFactory.hpp\n','')
with (s/p).open('a') as f:f.write('\ninstall(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/views/ViewFactory.hpp\n    DESTINATION include/lux/engine/editor/views COMPONENT lux_engine_editor_desktop)\n')
rpath=s/'editor/tests/architecture/rules.json';r=json.loads(rpath.read_text());l=r['editor_layering'];l['targets']['editor_configuration']['role']='PROVIDER'
for p,owner in [('editor/workbench/desktop/include/lux/engine/editor/views/ViewFactory.hpp','view_host'),('editor/workbench/desktop/src/ViewFactory.cpp','view_host'),('editor/workbench/scene/include/lux/engine/editor/scene/ConfigurationEditor.hpp','scene_ui'),('editor/workbench/scene/include/lux/engine/editor/scene/ConfigurationForm.hpp','scene_ui'),('editor/workbench/scene/src/ConfigurationEditor.cpp','scene_ui')]:l['files'][p]=[owner]
for scope in ['scene_ui','material_ui','flow_ui','editor_scene_views_test']:
 r[scope]['closure']['editor_configuration']={'path':'editor/authoring/configuration'}
if 'editor_configuration' not in r['scene_ui']['direct']:r['scene_ui']['direct'].append('editor_configuration')
for scope in ['view_host','desktop_shell']:
 for h in ['lux/engine/object/ObjectEvent.hpp','lux/engine/editor/views/ViewFactory.hpp','lux/cxx/core/move_only_function.hpp','lux/cxx/compile_time/TypeToken.hpp','span']:
  if h not in r[scope]['headers']:r[scope]['headers'].append(h)
for h in ['lux/engine/editor/configuration/ConfigurationValue.hpp','lux/engine/editor/scene/ConfigurationEditor.hpp','lux/engine/editor/scene/ConfigurationForm.hpp','lux/engine/editor/EditorError.hpp']:
 if h not in r['scene_ui']['headers']:r['scene_ui']['headers'].append(h)
rpath.write_text(json.dumps(r,indent=2)+'\n')
