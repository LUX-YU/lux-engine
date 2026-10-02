from pathlib import Path
import subprocess,json
r=Path('E:/SyncForder/CodeRepos/lux-engine-p12')
def change(path,old,new):
 p=r/path;t=p.read_text();assert old in t,(path,old);p.write_text(t.replace(old,new))
change('modules/function/ui/include/lux/engine/ui/Pane.hpp','        // transition: rooted construction is restricted to existing products until P12.\n        Pane(Root& parent, PaneId id, PaneTypeId type, std::string title);\n','')
change('modules/function/ui/include/lux/engine/ui/Pane.hpp','        Pane(object::LuxObject&, Root*, PaneId, PaneTypeId, std::string);\n','')
p=r/'modules/function/ui/src/Pane.cpp';t=p.read_text();start=t.index('    Pane::Pane(Root&');end=t.index('    Pane::~Pane()',start)
t=t[:start]+'''    Pane::Pane(Pane& parent, PaneId id, PaneTypeId type, std::string title)
        : Pane(parent.dispatcherRef(), std::move(id), std::move(type), std::move(title))
    {
        root_ = parent.attachedRoot();
        if (root_)
            root_->checkContentChange();
        parent.invalidatePreparation();
        attachTo(parent);
        if (root_)
            root_->registerPane(*this);
    }

'''+t[end:];p.write_text(t)
change('modules/function/ui/src/Root.cpp','        // The rooted destructor adapter keeps its established notification order until P12.\n        if (notify)\n            static_cast<void>(emit(objectRemoved, static_cast<object::LuxObject*>(&pane)));\n','')
change('modules/function/ui/src/Root.cpp','        pane.focused_ = pane.hovered_ = false;\n','        pane.focused_ = pane.hovered_ = false;\n        if (notify)\n            static_cast<void>(emit(objectRemoved, static_cast<object::LuxObject*>(&pane)));\n')
change('modules/function/ui/src/Root.cpp','        // The rooted destructor adapter keeps its established notification order until P12.\n        if (notify)\n            static_cast<void>(emit(objectRemoved, static_cast<object::LuxObject*>(&element)));\n','')
change('modules/function/ui/src/Root.cpp','        element.hovered_ = false;\n','        element.hovered_ = false;\n        if (notify)\n            static_cast<void>(emit(objectRemoved, static_cast<object::LuxObject*>(&element)));\n')
# Only test assembly convenience; production factories remain detached and Host owns mounting.
p=r/'cmake/installed-consumers/common/UiTestContent.hpp';p.write_text(p.read_text()+'''
#include <lux/engine/ui/Root.hpp>
#include <cassert>
#include <type_traits>
namespace ui_test
{
    template<class Parent> decltype(auto) parent(Parent& value)
    {
        if constexpr (std::derived_from<Parent, lux::ui::Root>)
            return value.dispatcherRef();
        else
            return (value);
    }
    template<class Parent> void mount(Parent& parent, lux::ui::Pane& pane)
    {
        if constexpr (std::derived_from<Parent, lux::ui::Root>)
        {
            assert(!pane.attachedRoot());
            auto ready = parent.prepareMount(pane);
            assert(ready);
            assert(parent.commit(*ready));
        }
    }
}
''')
# Root tests use complete subtrees, then the real prepare/commit boundary.
p=r/'modules/function/ui/test/root.cpp';t=p.read_text().replace('Pane(root, lux::ui::PaneId','Pane(root.dispatcherRef(), lux::ui::PaneId')
t=t.replace('            assert(root.requestFocus(*fields_.front()));','            ui_test::mount(root, *this);\n            assert(root.requestFocus(*fields_.front()));')
t=t.replace('            replace();\n','            replace();\n            ui_test::mount(root, *this);\n',1)
t=t.replace('lux::ui::Pane(parent, lux::ui::PaneId{id}', 'lux::ui::Pane(ui_test::parent(parent), lux::ui::PaneId{id}')
t=t.replace('            setContent(probe_content_);','            setContent(probe_content_);\n            ui_test::mount(parent, *this);')
t=t.replace('        Probe sibling(**first, "sibling");','        static_assert(!std::is_constructible_v<ui::Pane, ui::Root&, ui::PaneId, ui::PaneTypeId, std::string>);\n        Probe sibling(**first, "sibling");')
p.write_text(t)
for file in ['elements.hpp','input_checks.hpp']:
 p=r/'modules/function/ui/test'/file;t=p.read_text();t=t.replace('#pragma once','#pragma once\n#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"')
 if file=='elements.hpp':
  t=t.replace('ui::Pane(root,','ui::Pane(root.dispatcherRef(),').replace('            setContent(*item);','            setContent(*item);\n            ui_test::mount(root, *this);',1)
  t=t.replace('ui::Pane pane(root,','ui::Pane pane(root.dispatcherRef(),')
  line='        ui::Pane pane(root.dispatcherRef(), ui::PaneId{"layout"}, ui::PaneTypeId{"test"}, "Layout");'
  t=t.replace(line,line+'\n        ui_test::mount(root, pane);')
 else:
  t=t.replace('ui::Pane(parent,','ui::Pane(ui_test::parent(parent),').replace('            setContent(content);','            setContent(content);\n            ui_test::mount(parent, *this);')
 p.write_text(t)
change('cmake/installed-consumers/common/UiDrawPane.hpp','#include <utility>','#include <utility>\n#include "UiTestContent.hpp"')
change('cmake/installed-consumers/common/UiDrawPane.hpp','Pane(root,','Pane(root.dispatcherRef(),')
change('cmake/installed-consumers/common/UiDrawPane.hpp','        setContent(content_);','        setContent(content_);\n        ui_test::mount(root, *this);')
change('editor/tests/integration/scene_views/unified_ui.cpp','Pane(root,','Pane(root.dispatcherRef(),')
change('editor/tests/integration/scene_views/unified_ui.cpp','            setContent(probe_content_);','            setContent(probe_content_);\n            ui_test::mount(root, *this);')
p=r/'editor/tests/integration/scene_views/unified_ui.cpp';t=p.read_text()
import re
t=re.sub(r'(    ui::Pane (\w+)\()\*root,(.*?;)',lambda m:m[1]+'root->dispatcherRef(),'+m[3]+'\n    ui_test::mount(*root, '+m[2]+');',t)
p.write_text(t)
change('editor/tests/integration/scene_views/DraftSources.hpp','// Controlled input','#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"\n\n// Controlled input')
change('editor/tests/integration/scene_views/DraftSources.hpp','Pane(f.desktop->root(),','Pane(f.desktop->root().dispatcherRef(),')
change('editor/tests/integration/scene_views/DraftSources.hpp','            setContent(probe);','            setContent(probe);\n            ui_test::mount(f.desktop->root(), *this);')
p=r/'editor/tests/integration/project/scene_configuration.cpp';t=p.read_text();t='#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"\n'+t
t=t.replace('ui::Pane pane(**root,','ui::Pane pane((*root)->dispatcherRef(),')
t=t.replace('"Scene Configuration");','"Scene Configuration");\n    ui_test::mount(**root, pane);',1);p.write_text(t)
# The only consumer of the old generic close protocol is now the formal importer.
p=r/'editor/activities/project/include/lux/engine/editor/assets/AssetImporter.hpp';t=p.read_text().replace('#include <lux/engine/editor/CloseStatus.hpp>','#include <lux/engine/editor/EditorError.hpp>')
t=t.replace('    struct ModelImportRequest final','''    enum class EAssetImportCloseState : std::uint8_t { OPEN, CLOSING, CLOSED };
    struct AssetImportCloseStatus final
    {
        EAssetImportCloseState state{EAssetImportCloseState::OPEN};
        std::string waiting_for;
        EditorResult<void> progress;
    };
    struct ModelImportRequest final''').replace('[[nodiscard]] CloseStatus closeStatus()', '[[nodiscard]] AssetImportCloseStatus closeStatus()');p.write_text(t)
for file in ['editor/activities/project/src/AssetImporter.cpp','editor/activities/project/test/project_publication_operation.cpp','editor/application/src/EditorLifecycle.cpp']:
 p=r/file;t=p.read_text().replace('CloseStatus AssetImporter', 'AssetImportCloseStatus AssetImporter').replace('        CloseStatus closeStatus()', '        AssetImportCloseStatus closeStatus()')
 t=t.replace('ECloseState::','assets::EAssetImportCloseState::' if '/application/' in file else 'EAssetImportCloseState::');p.write_text(t)
change('editor/editing/CMakeLists.txt','    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/CloseStatus.hpp\n','')
subprocess.run(['git','rm','editor/editing/include/lux/engine/editor/CloseStatus.hpp'],cwd=r,check=True)
change('AGENTS.md','`app/context/ui/tools/launcher/metadata/plugins` 中仍有已登记的 P11/P12 旧产品消费者，\n新正式路径不得依赖它们。公开逻辑 include 和安装包不随物理目录迁移改名。','P12 已移除旧产品根与兼容接线；正式路径只能依赖相应层的真实 provider。\n公开逻辑 include 和安装包不随物理目录迁移改名。')
p=r/'editor/tests/architecture/rules.json';data=json.loads(p.read_text());data['editor_layering']['files']={k:v for k,v in data['editor_layering']['files'].items() if (r/k).exists()};p.write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n')
