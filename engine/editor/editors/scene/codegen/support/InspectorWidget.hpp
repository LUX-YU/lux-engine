#pragma once
// Editor generation support: included privately by generated Editor translation units only.
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <iterator>
#include <lux/engine/editor/gui/scene/InspectorInteraction.hpp>
#include <type_traits>
#include <utility>

namespace lux::editor::gui
{
    struct DefaultWidgetTag
    {
    };
    template <class T, class Tag = DefaultWidgetTag> struct InspectorWidget;

    struct InspectorField final
    {
        const char *label;
        const char *tooltip{};
        double speed{0.1};
        bool read_only{};
        std::uint32_t asset_magic{};
    };

    namespace generated_support
    {
        inline void merge(lux::ui::EditResult &result, lux::ui::EditResult next) noexcept
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
        struct FieldScope final
        {
            explicit FieldScope(const InspectorField &field, bool read_only, const char *identity = nullptr)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(field.label);
                ImGui::TableSetColumnIndex(1);
                ImGui::PushID(identity ? identity : field.label);
                ImGui::BeginDisabled(read_only || field.read_only);
            }
            ~FieldScope()
            {
                ImGui::EndDisabled();
                ImGui::PopID();
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
        struct TreeScope final
        {
            bool open;
            explicit TreeScope(const char *label) : open(ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {}
            ~TreeScope()
            {
                if (open)
                {
                    ImGui::TreePop();
                }
            }
        };
        template <class T> struct KeyDraft final
        {
            T original{}, value{};
            bool active{};
        };
        template <class Container, class Key>
        bool equivalentKey(const Container &container, const Key &first, const Key &second)
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
        template <class Container, class Mutation>
        bool prepareContainer(Container &value, InspectorInteraction &state, Mutation mutation) noexcept
        {
            Container prepared(value);
            if (!mutation(prepared))
            {
                state.fail("The container change is invalid or duplicates an existing key.");
                return false;
            }
            static_assert(std::is_nothrow_swappable_v<Container>);
            using std::swap;
            swap(value, prepared);
            // Scratch may own the container being edited (for example an insertion key).
            // Its lifetime ends with the Pane interaction, never inside this mutation.
            return true;
        }
    } // namespace generated_support
} // namespace lux::editor::gui
