#include <lux/engine/editor/inspector/FirstPartyComponentEditors.hpp>
#include <lux/engine/editor/inspector/GeneratedFieldSpec.hpp>

int main()
{
    auto bindings = lux::editor::inspector::buildFirstPartyComponentEditorBindings();
    const lux::editor::inspector::GeneratedFieldSpec spec{
        "value",
        "Value",
        {},
        lux::editor::inspector::EGeneratedWidget::DRAG,
        0.25,
        -1.0,
        1.0,
        {},
        false
    };
    return bindings && bindings->all().size() == 5U && spec.speed == 0.25 ? 0 : 1;
}
