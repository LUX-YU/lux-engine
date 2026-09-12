#pragma once
#include <InspectorWidget.hpp>
#include <lux/engine/description/Visual.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>

namespace lux::editor::ui
{
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
    template<> struct InspectorWidget<lux::simulation::ecs::Entity>
    {
        static lux::ui::EditResult draw(lux::simulation::ecs::Entity &value, InspectorInteraction &,
                                        const InspectorField &)
        {
            if (value == lux::simulation::ecs::NullEntity) ImGui::TextUnformatted("<none>");
            else ImGui::Text("Entity %u", entt::to_integral(value));
            return {};
        }
    };
    template<> struct InspectorWidget<lux::rdesc::MeshVisualDescription>
    {
        static lux::ui::EditResult draw(lux::rdesc::MeshVisualDescription &value, InspectorInteraction &state,
                                        const InspectorField &)
        {
            using namespace generated_support;
            {
                FieldScope field{{"Mesh"}, true};
                InspectorWidget<lux::asset::AssetId>::draw(value.mesh, state, {"Mesh"});
            }
            {
                FieldScope field{{"Material"}, true};
                InspectorWidget<lux::asset::AssetId>::draw(value.material, state, {"Material"});
            }
            FieldScope flags{{"Flags"}, true};
            ImGui::Checkbox("Visible", &value.visible);
            ImGui::Checkbox("Cast shadow", &value.cast_shadow);
            ImGui::Checkbox("Receive shadow", &value.receive_shadow);
            return {};
        }
    };
    // The existing Light author contract controls which fields are enabled, independently of type support.
    template<> struct InspectorWidget<lux::rdesc::LightDescription>
    {
        static lux::ui::EditResult draw(lux::rdesc::LightDescription &value, InspectorInteraction &state,
                                        const InspectorField &spec)
        {
            using namespace generated_support;
            lux::ui::EditResult result;
            constexpr std::array labels{"Directional", "Point", "Spot", "Area"};
            {
                FieldScope field{{"Type"}, state.read_only};
                const auto index = static_cast<unsigned>(value.type);
                const auto *selected = index < labels.size() ? labels[index] : "<unnamed value>";
                if (ImGui::BeginCombo("##type", selected))
                {
                    for (unsigned i = 0; i < labels.size(); ++i)
                        if (ImGui::Selectable(labels[i], i == index))
                        {
                            value.type = static_cast<lux::rdesc::ELightType>(i);
                            merge(result, immediate(true));
                        }
                    ImGui::EndCombo();
                }
            }
            {
                FieldScope field{{"Color"}, state.read_only};
                merge(result, edited(ImGui::ColorEdit3("##color", value.color.data())));
            }
            const auto scalar = [&](const char *name, float &number) {
                FieldScope field{{name}, state.read_only};
                merge(result, edited(ImGui::DragFloat("##value", &number, static_cast<float>(spec.speed))));
            };
            scalar("Intensity", value.intensity);
            scalar("Range", value.range);
            {
                FieldScope field{{"Cast shadow"}, state.read_only};
                merge(result, immediate(ImGui::Checkbox("##shadow", &value.cast_shadow)));
            }
            return result;
        }
    };
} // namespace lux::editor::ui
