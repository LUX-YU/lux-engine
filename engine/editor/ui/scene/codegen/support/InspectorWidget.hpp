#pragma once
// Editor generation support: included privately by generated Editor translation units only.
#include <lux/engine/editor/ui/scene/InspectorInteraction.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <type_traits>
#include <utility>
#if defined(LUX_EDITOR_INSPECTOR_TEST_PROBE)
#include <InspectorTestProbe.hpp>
#endif

namespace lux::editor::ui
{
    struct DefaultWidgetTag {};
    template<class T, class Tag = DefaultWidgetTag> struct InspectorWidget;

    struct InspectorField final
    {
        const char *label;
        const char *tooltip{};
        double speed{0.1};
        bool read_only{};
    };

    namespace generated_support
    {
        inline void traceItem(const char *label = "value") noexcept
        {
#if defined(LUX_EDITOR_INSPECTOR_TEST_PROBE)
            inspector_test::item(label, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
#endif
        }
        inline void merge(lux::ui::EditResult &result, lux::ui::EditResult next) noexcept
        {
            result.changed |= next.changed;
            result.began |= next.began;
            result.committed |= next.committed;
            result.cancelled |= next.cancelled;
        }
        inline lux::ui::EditResult edited(bool changed) noexcept
        {
            traceItem();
            return {changed, ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit(), false};
        }
        inline lux::ui::EditResult immediate(bool changed) noexcept
        {
            traceItem();
            return {changed, changed, changed, false};
        }
        struct FieldScope final
        {
            explicit FieldScope(const InspectorField &field, bool read_only, const char *identity = nullptr)
            {
#if defined(LUX_EDITOR_INSPECTOR_TEST_PROBE)
                inspector_test::push(field.label);
#endif
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
#if defined(LUX_EDITOR_INSPECTOR_TEST_PROBE)
                inspector_test::pop();
#endif
            }
        };
        struct IdScope final
        {
            explicit IdScope(int id) { ImGui::PushID(id); }
            ~IdScope() { ImGui::PopID(); }
        };
        struct TreeScope final
        {
            bool open;
            explicit TreeScope(const char *label) : open(ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {}
            ~TreeScope()
            {
                if (open) ImGui::TreePop();
            }
        };
        template<class T> struct KeyDraft final
        {
            T original{}, value{};
            bool active{};
        };
        template<class Container, class Key>
        bool equivalentKey(const Container &container, const Key &first, const Key &second)
        {
            if constexpr (requires { container.key_comp(); })
                return !container.key_comp()(first, second) && !container.key_comp()(second, first);
            else return container.key_eq()(first, second);
        }
        template<class Container, class Mutation>
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
} // namespace lux::editor::ui
