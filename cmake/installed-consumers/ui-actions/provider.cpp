#include <lux/engine/editor/EditorMenuComposition.hpp>

#if defined(_WIN32)
#define MENU_EXPORT __declspec(dllexport)
#else
#define MENU_EXPORT __attribute__((visibility("default")))
#endif

extern "C" MENU_EXPORT void contributeEditorMenu(lux::editor::EditorMenuComposition& composition) noexcept
{
    using namespace lux;
    const ui::MenuId menu{"menu.product"};
    const editor::MenuGroupId group{"group.external"};
    const ui::CommandId action{"external.action"};
    composition.actions.push_back({action, "External action", "Ctrl+P", {ui::EKey::P, true}});
    composition.groups.push_back({menu, group, {{}, editor::MenuGroupId{"group.host"}}});
    composition.contributions.push_back({menu, group, action, {ui::CommandId{"host.action"}}});
}
