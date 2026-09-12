#pragma once
#include "InspectorFixture.hpp"
#include <InspectorWidget.hpp>
#include <numbers>
namespace lux::editor::ui
{
    template<> struct InspectorWidget<inspector_fixture::Angle>
    {
        static lux::ui::EditResult draw(inspector_fixture::Angle &value, InspectorInteraction &state,
                                        const InspectorField &)
        {
            auto degrees = value.radians * 180.0 / std::numbers::pi;
            const auto result = generated_support::edited(
                ImGui::InputDouble("Degrees", &degrees, 1, 10, "%.3f"));
            if (result.changed)
            {
                if (!std::isfinite(degrees))
                {
                    state.fail("Angle must be finite.");
                    return {};
                }
                value.radians = degrees * std::numbers::pi / 180.0;
            }
            return result;
        }
    };
}
