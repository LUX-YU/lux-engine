from pathlib import Path
import re,json
s=Path('E:/SyncForder/CodeRepos/lux-engine-p12')
old=s/'editor/metadata/src/SceneRegistrations.cpp'
body=old.read_text();body=body[body.index('    lux::project::PluginResult'):body.rfind('}')]
body=body.replace('sceneRegistrations(', 'readSceneRegistrations(')
p=s/'engine/project/plugins/rendering/src/PluginRendering.cpp';text=p.read_text();pos=text.rfind('}');p.write_text(text[:pos]+body+text[pos:])
p=s/'engine/project/plugins/rendering/include/lux/engine/project/PluginRendering.hpp';text=p.read_text();text=text.replace('#include <lux/engine/project/PluginLibrary.hpp>','#include <lux/engine/project/PluginLibrary.hpp>\n#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>\n#include <lux/engine/simulation/SimulationSystemRegistry.hpp>')
pos=text.rfind('}');text=text[:pos]+'''    // Complete immutable scene assembly values from already loaded runtime plugins. This does not
    // load editor extensions and remains usable by a graphical player.
    struct SceneRegistrations final
    {
        simulation::ecs::ComponentSchemaSet components;
        std::shared_ptr<const simulation::SimulationSystemRegistry> simulation_systems;
        std::vector<scene::SceneSystemRegistration> scene_systems;
        std::vector<render::RenderFeatureRegistration> features;
        std::vector<scene::RenderFeatureSceneBinding> render_bindings;
    };
    [[nodiscard]] PluginResult<SceneRegistrations> readSceneRegistrations(
        std::span<const simulation::ecs::ComponentSchema> additional = {},
        std::span<const std::shared_ptr<const PluginLibrary>> plugins = {}
    );
'''+text[pos:];p.write_text(text)
for base in ['editor','cmake/installed-consumers']:
 for p in (s/base).rglob('*'):
  if not p.is_file() or p.suffix not in ['.hpp','.cpp']:continue
  if p == old or p.name == 'SceneRegistrations.hpp':continue
  text=p.read_text(encoding='utf-8-sig');prior=text
  text=text.replace('lux/engine/editor/metadata/SceneRegistrations.hpp','lux/engine/project/PluginRendering.hpp')
  if 'struct SceneRegistrations;' in text:
   text=text.replace('    struct SceneRegistrations;\n','')
   text=text.replace('#pragma once','#pragma once\nnamespace lux::project { struct SceneRegistrations; }')
  text=re.sub(r'(?<![\w:])SceneRegistrations\b','lux::project::SceneRegistrations',text)
  text=text.replace('lux::editor::sceneRegistrations({},','lux::project::readSceneRegistrations({},')
  text=text.replace('editor::sceneRegistrations({},','lux::project::readSceneRegistrations({},')
  text=text.replace('= sceneRegistrations({},','= lux::project::readSceneRegistrations({},')
  text=text.replace('= sceneRegistrations(consumer::schemas(),','= lux::project::readSceneRegistrations(consumer::schemas(),')
  if text!=prior:p.write_text(text,encoding='utf-8')
old.unlink();(s/'editor/metadata/include/lux/engine/editor/metadata/SceneRegistrations.hpp').unlink()
p=s/'editor/metadata/CMakeLists.txt';text=p.read_text().replace(' src/SceneRegistrations.cpp','').replace('target_link_libraries(editor_metadata PRIVATE lux::engine::project::project_plugin_rendering\n    PUBLIC','target_link_libraries(editor_metadata PUBLIC lux::engine::project::project_plugin_rendering');p.write_text(text)
