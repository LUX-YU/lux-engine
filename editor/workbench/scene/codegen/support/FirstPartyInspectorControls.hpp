#pragma once
#include <InspectorControl.hpp>
#include <lux/engine/description/Visual.hpp>
#include <lux/engine/editor/project/AssetPickerElement.hpp>

#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>

namespace lux::editor::ui
{
    class MeshVisualControl final : public lux::ui::Element
    {
    public:
        object::TSignal<lux::ui::EditResult> edited{*this};
        template <class Interaction>
        MeshVisualControl(
            lux::ui::Element& parent,
            lux::ui::ElementId id,
            Interaction& interaction,
            typename Interaction::Status& status,
            lux::rdesc::MeshVisualDescription value
        )
            : Element(parent, std::move(id)), value_(value),
              layout_(*this, lux::ui::ElementId{"fields"}, lux::ui::ELayoutType::FORM),
              mesh_label_(layout_, lux::ui::ElementId{"mesh-label"}, "Mesh"), mesh_(
                                                                                  layout_,
                                                                                  lux::ui::ElementId{"mesh"},
                                                                                  interaction.assetCatalog(),
                                                                                  lux::asset::MeshAsset::primary_magic,
                                                                                  value.mesh
                                                                              ),
              material_label_(layout_, lux::ui::ElementId{"material-label"}, "Material"),
              material_(
                  layout_,
                  lux::ui::ElementId{"material"},
                  interaction.assetCatalog(),
                  lux::asset::MaterialAsset::primary_magic,
                  value.material
              ),
              visible_label_(layout_, lux::ui::ElementId{"visible-label"}, "Visible"),
              visible_(layout_, lux::ui::ElementId{"visible"}, "", value.visible),
              cast_label_(layout_, lux::ui::ElementId{"cast-label"}, "Cast shadow"),
              cast_(layout_, lux::ui::ElementId{"cast"}, "", value.cast_shadow),
              receive_label_(layout_, lux::ui::ElementId{"receive-label"}, "Receive shadow"),
              receive_(layout_, lux::ui::ElementId{"receive"}, "", value.receive_shadow)
        {
            const auto changed = [this](lux::ui::EditResult change) noexcept {
                value_ = {mesh_.value(), material_.value(), visible_.value(), cast_.value(), receive_.value()};
                static_cast<void>(emit(edited, change));
            };
            connections_[0] = generated_support::takeConnection<Interaction>(
                connect(&mesh_, &project::AssetPickerElement::edited, changed),
                status
            );
            connections_[1] = generated_support::takeConnection<Interaction>(
                connect(&material_, &project::AssetPickerElement::edited, changed),
                status
            );
            connections_[2] = generated_support::takeConnection<Interaction>(
                connect(&visible_, &lux::ui::CheckBox::edited, changed),
                status
            );
            connections_[3] = generated_support::takeConnection<Interaction>(
                connect(&cast_, &lux::ui::CheckBox::edited, changed),
                status
            );
            connections_[4] = generated_support::takeConnection<Interaction>(
                connect(&receive_, &lux::ui::CheckBox::edited, changed),
                status
            );
        }
        const lux::rdesc::MeshVisualDescription& value() const noexcept
        {
            return value_;
        }
        void setValue(lux::rdesc::MeshVisualDescription value) noexcept
        {
            value_ = value;
            mesh_.setValue(value.mesh);
            material_.setValue(value.material);
            visible_.setValue(value.visible);
            cast_.setValue(value.cast_shadow);
            receive_.setValue(value.receive_shadow);
        }

    private:
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            return layout_.sizeHint();
        }
        lux::ui::SizeHint measureContent(float width) noexcept override
        {
            return layout_.measure(width);
        }
        void arrangeContent() noexcept override
        {
            layout_.arrange({{}, rect().size});
        }
        void draw() noexcept override
        {
            drawChild(layout_);
        }
        lux::rdesc::MeshVisualDescription value_;
        lux::ui::Layout layout_;
        lux::ui::Label mesh_label_;
        project::AssetPickerElement mesh_;
        lux::ui::Label material_label_;
        project::AssetPickerElement material_;
        lux::ui::Label visible_label_;
        lux::ui::CheckBox visible_;
        lux::ui::Label cast_label_;
        lux::ui::CheckBox cast_;
        lux::ui::Label receive_label_;
        lux::ui::CheckBox receive_;
        std::array<object::Connection, 5> connections_;
    };
    template <> struct TInspectorControl<lux::simulation::ecs::Entity>
    {
        static lux::ui::EditResult draw(lux::simulation::ecs::Entity& value, auto&, const InspectorField&)
        {
            ImGui::TextUnformatted(value == lux::simulation::ecs::NullEntity ? "<none>" : "Parent object");
            return {};
        }
    };
    template <> struct TInspectorControl<lux::rdesc::LightDescription>
    {
        static lux::ui::EditResult draw(lux::rdesc::LightDescription& value, auto& state, const InspectorField& spec)
        {
            using namespace generated_support;
            lux::ui::EditResult result;
            constexpr std::array labels{"Directional", "Point", "Spot", "Area"};
            {
                FieldScope field{{"Type"}, state};
                const auto index = static_cast<unsigned>(value.type);
                const auto* selected = index < labels.size() ? labels[index] : "<unnamed value>";
                if (ImGui::BeginCombo("##type", selected))
                {
                    for (unsigned i = 0; i < labels.size(); ++i)
                    {
                        if (ImGui::Selectable(labels[i], i == index) && !state.readOnly())
                        {
                            value.type = static_cast<lux::rdesc::ELightType>(i);
                            merge(result, immediate(true));
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            {
                FieldScope field{{"Color"}, state};
                const auto before = value.color;
                const bool changed = ImGui::ColorEdit3("##color", value.color.data());
                merge(result, edited(state.changed(value.color, before, changed)));
            }
            const auto scalar = [&](const char* name, float& number) {
                FieldScope field{{name}, state};
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
                FieldScope field{{"Cast shadow"}, state};
                const auto before = value.cast_shadow;
                const bool changed = ImGui::Checkbox("##shadow", &value.cast_shadow);
                merge(result, immediate(state.changed(value.cast_shadow, before, changed)));
            }
            return result;
        }
    };
} // namespace lux::editor::ui
