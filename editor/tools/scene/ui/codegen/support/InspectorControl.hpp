#pragma once
// Editor generation support: included privately by generated Editor translation units only.
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <algorithm>
#include <cmath>
#include <exception>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <iterator>

#include <ranges>
#include <type_traits>
#include <utility>

namespace lux::editor::ui
{
    struct DefaultControlTag
    {};
    template <class T, class Tag = DefaultControlTag> struct TInspectorControl;

    struct InspectorField final
    {
        const char* label;
        const char* tooltip{};
        double speed{0.1};
        bool read_only{};
        std::uint32_t asset_magic{};
    };

    namespace generated_support
    {
        // A local factory result, never retained by the UI tree. Callers check it before publication.
        template <class Interaction>
        object::Connection takeConnection(
            object::LuxObject::ConnectResult connected,
            typename Interaction::Status& status
        )
        {
            if (connected)
                return std::move(*connected);
            if (status)
                status = Interaction::connectionFailure(connected.error());
            return {};
        }
        inline void merge(lux::ui::EditResult& result, lux::ui::EditResult next) noexcept
        {
            result.changed |= next.changed;
            result.began |= next.began;
            result.committed |= next.committed;
            result.cancelled |= next.cancelled;
        }
        inline lux::ui::EditResult edited(bool changed) noexcept
        {
            return {changed, ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), false};
        }
        inline lux::ui::EditResult immediate(bool changed) noexcept
        {
            return {changed, changed, changed, false};
        }
        inline lux::ui::EditResult editString(const char* label, std::string& value, auto& state)
        {
            const auto flags = state.readOnly() ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None;
            return edited(ImGui::InputText(label, &value, flags));
        }

        template <class Interaction> struct FieldScope final
        {
            Interaction& interaction;
            bool previous;
            explicit FieldScope(const InspectorField& field, Interaction& state, const char* identity = nullptr)
                : interaction(state), previous(state.readOnly())
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(field.label);
                ImGui::TableSetColumnIndex(1);
                ImGui::PushID(identity ? identity : field.label);
                interaction.setReadOnly(previous || field.read_only);
            }
            ~FieldScope()
            {
                interaction.setReadOnly(previous);
                ImGui::PopID();
            }
        };
        struct ReadOnlyScope final
        {
            explicit ReadOnlyScope(bool disabled)
            {
                ImGui::BeginDisabled(disabled);
            }
            ~ReadOnlyScope()
            {
                ImGui::EndDisabled();
            }
        };
        struct IdScope final
        {
            explicit IdScope(int id)
            {
                ImGui::PushID(id);
            }
            ~IdScope()
            {
                ImGui::PopID();
            }
        };
        template <class Container, class Key>
        bool equivalentKey(const Container& container, const Key& first, const Key& second)
        {
            if constexpr (requires { container.key_comp(); })
            {
                return !container.key_comp()(first, second) && !container.key_comp()(second, first);
            }
            else
            {
                return container.key_eq()(first, second);
            }
        }
    } // namespace generated_support
} // namespace lux::editor::ui
