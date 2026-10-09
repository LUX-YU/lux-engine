#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui_internal.h>
#include <limits>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/detail/Context.hpp>
#include <lux/engine/ui/detail/ContextActivation.hpp>
#include <lux/engine/ui/detail/Contract.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>
#include <variant>

namespace lux::ui
{
    namespace detail
    {
        struct Mutation final
        {
            bool& active;

            explicit Mutation(bool& flag) noexcept : active(flag)
            {
                active = true;
            }

            ~Mutation()
            {
                active = false;
            }
        };

        template <class Visit> void visitSubtree(object::LuxObject& root, Visit&& visit) noexcept
        {
            auto* node = &root;
            for (;;)
            {
                visit(*node);
                if (node->firstChild())
                {
                    node = node->firstChild();
                }
                else
                {
                    while (node != &root && !node->nextSibling())
                    {
                        node = node->parent();
                    }
                    if (node == &root)
                    {
                        return;
                    }
                    node = node->nextSibling();
                }
            }
        }

    } // namespace detail

    using detail::Mutation;
    using detail::visitSubtree;

    struct DockData final
    {
        struct Node final
        {
            EDockSplit split{EDockSplit::LEAF};
            std::uint32_t first{UINT32_MAX}, second{UINT32_MAX};
            float ratio{0.5F};
            std::vector<PaneId> panes;
        };

        std::vector<Node> nodes;
        std::vector<DockSurface> surfaces;
        std::vector<std::uint32_t> order;
        std::vector<ImGuiID> ids;
    };

    struct Root::Impl final
    {
        struct StoredTarget final
        {
            object::ObjectId root;
            PaneId pane;
            object::ObjectId object;
            bool operator==(const StoredTarget&) const noexcept = default;
        };

        struct Target final
        {
            Pane* pane{};
            Element* element{};
            Target() noexcept = default;

            Target(Pane* value) noexcept : pane(value) {}

            Target(Element* value) noexcept : element(value) {}

            Target(std::nullptr_t) noexcept {}

            [[nodiscard]] object::LuxObject* object() const noexcept
            {
                return element ? static_cast<object::LuxObject*>(element) : pane;
            }

            [[nodiscard]] Pane* containingPane() const noexcept
            {
                return element ? &element->pane() : pane;
            }

            [[nodiscard]] bool visible() const noexcept
            {
                return element ? element->displayed() : pane && pane->visible();
            }

            explicit operator bool() const noexcept
            {
                return object() != nullptr;
            }

            [[nodiscard]] object::LuxObject& operator*() const noexcept
            {
                return *object();
            }

            bool operator==(const Target&) const noexcept = default;
        };

        struct CommandExecution final
        {
            CommandId command;
        };

        struct DeferredMutation final
        {
            ChangeCallback apply{};
        };

        using VUiSafePointPayload = std::variant<DeferredMutation, CommandExecution>;

        struct UiSafePointAction final
        {
            StoredTarget target;
            VUiSafePointPayload payload;
        };

        struct MenuState final
        {
            std::vector<MenuItem> items;
            Pane* pane{};
            Element* element{};
            bool open{};
            float height{};
        } menu_state;

        struct FocusState final
        {
            Pane *focused{}, *hovered{}, *pending_focus{};
            Target pointer_capture;
            Pane *draw_focused{}, *draw_hovered{};
            Element *focused_element{}, *hovered_element{}, *pending_element{};
            Element *draw_focused_element{}, *draw_hovered_element{};
            Pane* modal{};
        } focus_state;

        struct SafePointState final
        {
            std::vector<UiSafePointAction> pending;
            std::size_t batch_size{};
            object::LuxObject* active{};
        } safe_point_state;

        struct LayoutState final
        {
            std::size_t depth{};
            std::uint64_t epoch{};
        } layout_state;

        struct DockState final
        {
            bool enabled{};
            std::unique_ptr<DockData> pending;
        } dock_state;

        struct InputState final
        {
            ImGuiKeyChord modifiers{};
            bool composing{};
        } input_state;

        static StoredTarget store(Root&, Pane&, object::LuxObject&) noexcept;
        static object::LuxObject* resolve(Root&, StoredTarget) noexcept;
        void queueChange(StoredTarget, ChangeCallback) noexcept;
        void cancelActions(object::LuxObject&) noexcept;
        void applyPendingChanges(Root&) noexcept;
        void drawMenu(Root&) noexcept;
        void drawMenuItems(Root&, std::span<const MenuItem>) noexcept;
        void routeCommand(Root&, object::LuxObject*, Command&) noexcept;
        void menuCommand(Root&, Command&) noexcept;
        bool shortcut(Root&, const Key&) noexcept;
        object::ObjectId command_fallback;
        UpdateStatistics statistics;
        std::unique_ptr<detail::Context> context;
        window::LuxWindow* window{};
        std::size_t pane_capacity{65536};
        cxx::SlotKeyAutoSparseSet<PaneId, std::unique_ptr<Pane>> panes;
        object::LuxObject* active_update{};
        bool committing_structure{}, drawing{}, updating{};
    };
} // namespace lux::ui
