#pragma once
#include <InspectorWidget.hpp>
#include <lux/engine/description/Visual.hpp>
#include <lux/engine/editor/gui/asset/AssetReference.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>

namespace lux::editor::gui
{
    template <> struct InspectorWidget<lux::asset::AssetId>
    {
        static lux::ui::EditResult draw(lux::asset::AssetId &value, InspectorInteraction &state,
                                        const InspectorField &field)
        {
            const auto original = value;
            const auto result = drawAssetReference(state.document.project(), value, field.asset_magic);
            if (!result)
            {
                state.fail(result.error().message.empty()
                               ? "The asset belongs to another project, changed during the drag, or has the wrong type."
                               : result.error().message.c_str());
                return {};
            }
            return generated_support::immediate(state.changed(value, original, *result));
        }
    };
    template <> struct InspectorWidget<lux::simulation::ecs::Entity>
    {
        static lux::ui::EditResult draw(lux::simulation::ecs::Entity &value, InspectorInteraction &,
                                        const InspectorField &)
        {
            ImGui::TextUnformatted(value == lux::simulation::ecs::NullEntity ? "<none>" : "Parent object");
            return {};
        }
    };
    template <> struct InspectorWidget<lux::rdesc::MeshVisualDescription>
    {
        static lux::ui::EditResult draw(lux::rdesc::MeshVisualDescription &value, InspectorInteraction &state,
                                        const InspectorField &spec)
        {
            using namespace generated_support;
            lux::ui::EditResult result;
            {
                FieldScope field{{"Mesh", nullptr, 0.1, spec.read_only}, state.read_only};
                merge(result,
                      InspectorWidget<lux::asset::AssetId>::draw(
                          value.mesh, state, {"Mesh", nullptr, 0.1, false, lux::asset::MeshAsset::primary_magic}));
            }
            {
                FieldScope field{{"Material", nullptr, 0.1, spec.read_only}, state.read_only};
                merge(result, InspectorWidget<lux::asset::AssetId>::draw(
                                  value.material, state,
                                  {"Material", nullptr, 0.1, false, lux::asset::MaterialAsset::primary_magic}));
            }
            FieldScope flags{{"Flags", nullptr, 0.1, spec.read_only}, state.read_only};
            {
                const auto before = value.visible;
                const bool changed = ImGui::Checkbox("Visible", &value.visible);
                merge(result, immediate(state.changed(value.visible, before, changed)));
            }
            {
                const auto before = value.cast_shadow;
                const bool changed = ImGui::Checkbox("Cast shadow", &value.cast_shadow);
                merge(result, immediate(state.changed(value.cast_shadow, before, changed)));
            }
            {
                const auto before = value.receive_shadow;
                const bool changed = ImGui::Checkbox("Receive shadow", &value.receive_shadow);
                merge(result, immediate(state.changed(value.receive_shadow, before, changed)));
            }
            return result;
        }
    };
    template <> struct InspectorWidget<lux::rdesc::LightDescription>
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
                    {
                        if (ImGui::Selectable(labels[i], i == index) && state.beforeWrite())
                        {
                            value.type = static_cast<lux::rdesc::ELightType>(i);
                            merge(result, immediate(true));
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            {
                FieldScope field{{"Color"}, state.read_only};
                const auto before = value.color;
                const bool changed = ImGui::ColorEdit3("##color", value.color.data());
                merge(result, edited(state.changed(value.color, before, changed)));
            }
            const auto scalar = [&](const char *name, float &number)
            {
                FieldScope field{{name}, state.read_only};
                const auto before = number;
                const bool changed = ImGui::DragFloat("##value", &number, static_cast<float>(spec.speed));
                if (!std::isfinite(number))
                {
                    number = before;
                    state.fail("A finite value is required");
                }
                else
                {
                    merge(result, edited(state.changed(number, before, changed)));
                }
            };
            scalar("Intensity", value.intensity);
            scalar("Range", value.range);
            {
                FieldScope field{{"Cast shadow"}, state.read_only};
                const auto before = value.cast_shadow;
                const bool changed = ImGui::Checkbox("##shadow", &value.cast_shadow);
                merge(result, immediate(state.changed(value.cast_shadow, before, changed)));
            }
            return result;
        }
    };
} // namespace lux::editor::gui
