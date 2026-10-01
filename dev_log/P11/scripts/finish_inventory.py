from pathlib import Path
import json,re,subprocess
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11');w=Path(r'E:/SyncForder/CodeRepos/lux-engine/.internal/editor-redesign/P11')
items=json.loads((w/'source-map.json').read_text());rules=json.loads((s/'editor/tests/architecture/rules.json').read_text());providers=rules['editor_layering']['files'];graph=json.loads(Path(r'E:/SyncForder/CodeRepos/build/RelWithDebInfo/p11-dev/editor-architecture/targets.json').read_text())
tracked=subprocess.check_output(['git','ls-files','-co','--exclude-standard'],cwd=s,text=True).splitlines();texts={f:(s/f).read_text(errors='replace') for f in tracked if (s/f).is_file() and f.endswith(('.hpp','.cpp','.h','.cmake','CMakeLists.txt'))}
moved={
'editor/metadata/include/lux/engine/editor/metadata/ConfigurationValue.hpp':'editor/authoring/configuration/include/lux/engine/editor/configuration/ConfigurationValue.hpp',
'editor/metadata/src/ConfigurationValue.cpp':'editor/authoring/configuration/src/ConfigurationValue.cpp',
'editor/metadata/include/lux/engine/editor/metadata/EditorReflection.hpp':'editor/authoring/configuration/include/lux/engine/editor/configuration/EditorReflection.hpp',
'editor/metadata/src/EditorReflection.cpp':'editor/authoring/configuration/src/EditorReflection.cpp',
'editor/plugins/src/ConfigurationForm.hpp':'editor/workbench/scene/include/lux/engine/editor/scene/ConfigurationForm.hpp'}
precise={
'AssetEditorRegistration.hpp':('AssetEditorRegistration / OpenRequest / LoadRequest','EditorContext::openAsset, old SceneEditor/MaterialEditor/FlowForgeEditor factory registration','P12-B: OpenAndShow uses SessionFactorySnapshot + ViewFactorySnapshot; then remove old types'),
'CommandRegistration.hpp':('CommandRegistration::invoke / ECommandPhase','EditorContext::setCommands, EditorMenu, ProductCommands','P12-E/F: switch old executable to CommandRegistry/CommandMenu; delete invoke table'),
'ComponentEditorRegistry.hpp':('ComponentEditorRegistration / ComponentEditorRegistry','EditorContext::initialize, old InspectorPane / generated legacy inspection factories','P12-D/G: use formal InspectorComponent + InspectorFields everywhere; delete Registry'),
'ComponentEditorRegistry.cpp':('ComponentEditorRegistry::create/find','EditorContext and old SceneEditor editing/Inspector consumers','P12-D/G: finish formal Inspector consumers, remove legacy schema-to-Registry editing wrapper'),
'EditorPlugin.hpp':('EditorPlugin/loadEditorPlugins','EditorContext startup/updatePlugins, metadata plugin regression','P12-E/F: application loads EditorExtension V7 only; remove V6 loader'),
'EditorPlugin.cpp':('loadEditorPlugins/V6 validation','EditorContext startup/updatePlugins','P12-E/F: delete V6 loader after product switch; V6 remains rejection test only'),
'EditorPluginExports.hpp':('ConfigurationEditorRegistration / EditorPluginExports V6 fields','EditorContext; three old editor sidecars; old installed external-feature test','P12-F/G: switch sidecars/export and legacy consumers, delete V6 production header/symbol'),
'PaneRegistration.hpp':('PaneRegistration','PaneManager, ProductAssembly and settings/asset picker construction','P12-D/F: ViewFactorySnapshot + ViewHost; delete old registry'),
'PaneState.hpp':('PaneState','old Workspace/PaneManager/settings layout persistence','P12-C/G: formal Layout/Recovery + ViewHost bindings replace old view metadata'),
'SceneRegistrations.hpp':('SceneRegistrations / sceneRegistrations','EditorContext, old SceneEditor, ProjectCreationPane','P12-E/F: application assembles runtime registrations from engine PluginLibrary; no new inner dependency on this old aggregate'),
'SceneRegistrations.cpp':('sceneRegistrations schema/system/render assembly','EditorContext and ProjectCreationPane','P12-E/F: move sole assembly body to application alongside PluginLibrary composition; delete old aggregate and metadata provider'),
'ConfigurationForm.hpp':('ConfigurationForm / configurationEditor<T>','render_feature_meta V6 table field conversion','P11-D: moved one form/codec algorithm; P12-F deletes only private V6 field-layout conversion'),
'Physics2DEditorExports.cpp':('PhysicsConfigurationElement / createConfiguration / V6 export','physics2d_editor sidecar loaded only by old EditorContext','P12-D/F: move specific physics configuration UI into formal workbench provider; reuse engine physics codec and migrate sidecar ABI'),
'RenderSystemEditorExports.cpp':('V6 render-system configuration export','scene_render_meta sidecar loaded only by old EditorContext','P12-F: formal contribution table and existing SceneConfigurationElement; delete V6 definition'),
'RenderFeatureEditorExports.cpp':('legacyConfigurationEditor<T> / V6 export','render_feature_meta -> old EditorContext/ProjectCreationPane','P12-F: replace field-layout conversion/export with formal ConfigurationEditor contributions; shared form already unique'),
'EditorMenu.cpp':('Editor::Impl::menu/openMenu/executeMenuCommand','old lux_editor and baseline C03 probe','P11-F local registration pin fixes live C03; P12-E/F delete entire old menu after formal Shell switch'),
'ProductCommands.cpp':('ProjectCommands/rememberProject/newAsset/productCommands','old lux_editor executable only','P12-B/D/E: explicit OpenAndShow, project commands, recent-project persistence and Exit; remove old Context closures'),
'ProductAssembly.cpp':('productAssembly/product pane/asset registration','old lux_editor executable only','P12-D/F: formal builtin content/view contributions and application services; delete old assembly')}
for item in items:
 path=item['path'];name=Path(path).name;destination=moved.get(path,path)
 blob=subprocess.check_output(['git','show','22ab1a3:'+path],cwd=s,text=True,errors='replace')
 entry=precise.get(name)
 if not entry:
  group=path.split('/')[1]
  entry=(name.removesuffix('.cpp').removesuffix('.hpp'),{'app':'lux_editor/editor_app and old protocol test executables','context':'Editor/old tools/launcher/settings (EditorContext and PaneManager product protocol)','metadata':'old editor metadata/V6 tables and metadata regression consumers','plugins':'three old sidecar targets and plugin registration generators'}[group],{'app':'P12-E/F/G: replace actual executable with DesktopShell/ViewHost + application open/exit; migrate same tests then delete','context':'P12-B/C/D/F: formal activities own content, ViewHost owns windows, Workspace formal plan; delete old Context/PaneManager','metadata':'P12-F/G: remove final old registration consumers and corresponding target/package; retain engine runtime exports','plugins':'P12-F/G: switch sidecars to formal contributions and generated providers; remove old target/package outputs'}[group])
 item.update(symbol_scope=entry[0],last_product_consumer=entry[1],replacement_step=entry[2],final_path=destination,
  action='MOVED_ORIGINAL_DELETED' if path in moved else 'RETAINED_PRODUCT_OR_TEST',status='P11-F disposition; final qualification pending',deadline=None if path in moved else 'P12')
 item['declared_symbols']=sorted(set(re.findall(r'\b(?:class|struct|enum class)\s+(?:\w+_PUBLIC\s+)?(\w+)',blob)))
 item['providers']=providers.get(destination,[]) or [t['name'] for t in graph if Path(t['SOURCE_DIR'])==s/path.rsplit('/',1)[0] and name=='CMakeLists.txt']
 if not item['providers']:
  item['providers']=[{'app':'editor_app','context':'editor_context','metadata':'editor_metadata','plugins':'scene_render_meta/render_feature_meta/physics2d_editor'}[path.split('/')[1]]]
 logical=item.get('logical_include');item['include_consumers']=[f for f,text in texts.items() if logical and logical in text]
 item['target_consumers']=[t['name'] for t in graph if any(re.search(r'(?<!\w)'+re.escape(p)+r'(?!\w)',t.get('edges','')) for p in item['providers'])]
(w/'source-map.json').write_text(json.dumps(items,ensure_ascii=False,indent=2)+'\n')
ledger=w.parent/'migration-ledger.json';d=json.loads(ledger.read_text());d['closeout'].update(batch='F',status='IN_PROGRESS',sdk_lifetime_regression='Real SDK weak control-block unload failure recorded K-sdk-tests/K-sdk-debug; fix under validation');ledger.write_text(json.dumps(d,ensure_ascii=False,indent=2)+'\n')
print('P11 disposition:',len(items),'legacy files;',len(moved),'originals removed; each retained row names P12 consumer and replacement')
