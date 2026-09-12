#pragma once
#include <InspectorWidget.hpp>
namespace lux::editor::ui
{
    // An external Editor plugin supplies its own complex-type specialization at compilation.
    template<> struct InspectorWidget<lux::asset::AssetId>
    {
        static lux::ui::EditResult draw(lux::asset::AssetId &value, InspectorInteraction &state,
                                        const InspectorField &)
        {
            const auto path = state.asset_path ? state.asset_path(state.asset_source, value) :
                std::string{value.isNull() ? "<none>" : "<unresolved asset>"};
            ImGui::TextUnformatted(path.c_str());
            return {};
        }
    };
}
