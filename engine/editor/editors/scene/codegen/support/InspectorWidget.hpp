#pragma once
// Editor generation support: included privately by generated Editor translation units only.
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <iterator>
#include <lux/engine/editor/gui/scene/InspectorInteraction.hpp>
#include <ranges>
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
            bool &read_only;
            bool previous;
            explicit FieldScope(const InspectorField &field, bool &state_read_only, const char *identity = nullptr)
                : read_only(state_read_only), previous(state_read_only)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(field.label);
                ImGui::TableSetColumnIndex(1);
                ImGui::PushID(identity ? identity : field.label);
                read_only = previous || field.read_only;
            }
            ~FieldScope()
            {
                read_only = previous;
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
        inline bool readOnlyButton(const char *label, InspectorInteraction &state)
        {
            ReadOnlyScope scope{state.read_only};
            return ImGui::SmallButton(label);
        }
        inline bool readOnlyCheckbox(const char *label, bool *value, InspectorInteraction &state)
        {
            ReadOnlyScope scope{state.read_only};
            return ImGui::Checkbox(label, value);
        }
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

        inline constexpr std::size_t container_page_size = 64;

        template <class Range> struct ContainerPageState final
        {
            using Iterator = std::ranges::iterator_t<Range>;
            std::size_t first{}, anchor_offset{}, size{};
            std::uint64_t epoch{};
            const void *owner{};
            Iterator anchor{};
        };

        template <class Iterator> struct ContainerPage final
        {
            std::size_t first, end;
            Iterator begin;
        };

        struct PageBounds final
        {
            std::size_t first{}, end{};
        };

        inline PageBounds containerPageBounds(std::size_t count, InspectorInteraction &state)
        {
            auto &page = state.input<PageBounds>(ImGui::GetID("container-page"));
            const auto last = count ? (count - 1) / container_page_size : 0;
            auto selected = std::min(page.first / container_page_size, last);
            if (count > container_page_size)
            {
                ImGui::BeginDisabled(selected == 0);
                const bool previous = ImGui::SmallButton("Previous page");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(selected == last);
                const bool next = ImGui::SmallButton("Next page");
                ImGui::EndDisabled();
                if ((previous || next) && state.finish(state.document, true))
                {
                    selected = previous ? selected - 1 : selected + 1;
                }
                ImGui::SameLine();
                ImGui::Text("%zu / %zu", selected + 1, last + 1);
            }
            page.first = selected * container_page_size;
            page.end = std::min(page.first + container_page_size, count);
            return page;
        }

        template <class Range> auto containerPage(Range &value, InspectorInteraction &state)
        {
            const auto count = std::ranges::size(value);
            const auto bounds = containerPageBounds(count, state);
            auto &page = state.input<ContainerPageState<Range>>(ImGui::GetID("container-anchor"));
            page.first = bounds.first;
            if (page.owner != &value || page.epoch != state.containerEpoch() || page.size != count)
            {
                page.owner = &value;
                page.epoch = state.containerEpoch();
                page.size = count;
                page.anchor = std::ranges::begin(value);
                page.anchor_offset = 0;
            }
            if constexpr (std::ranges::random_access_range<Range>)
            {
                page.anchor = std::ranges::begin(value) + static_cast<std::ptrdiff_t>(page.first);
            }
            else
            {
                // Keep the page anchor, not the loop's advancing iterator. An idle
                // far page traverses only its visible items on subsequent frames.
                if (page.first < page.anchor_offset)
                {
                    page.anchor = std::ranges::begin(value);
                    page.anchor_offset = 0;
                }
                std::advance(page.anchor, static_cast<std::ptrdiff_t>(page.first - page.anchor_offset));
            }
            page.anchor_offset = page.first;
            return ContainerPage{bounds.first, bounds.end, page.anchor};
        }
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
            state.invalidateContainerIterators();
            // Scratch may own the container being edited (for example an insertion key).
            // Its lifetime ends with the Pane interaction, never inside this mutation.
            return true;
        }
    } // namespace generated_support
} // namespace lux::editor::gui
