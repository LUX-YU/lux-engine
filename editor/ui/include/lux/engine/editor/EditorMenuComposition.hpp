#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ui/Menu.hpp>

namespace lux::editor
{
    struct MenuGroupIdTag final
    {
    };

    using MenuGroupId = cxx::StableNameId<MenuGroupIdTag>;

    template <class Id> struct MenuOrder final
    {
        Id before;
        Id after;
        int priority{};
    };

    struct EditorMenu final
    {
        ui::MenuId id;
        std::string label;
        ui::MenuId parent;
        MenuOrder<ui::MenuId> order;
    };

    struct EditorMenuGroup final
    {
        ui::MenuId menu;
        MenuGroupId id;
        MenuOrder<MenuGroupId> order;
    };

    struct MenuContribution final
    {
        ui::MenuId menu;
        MenuGroupId group;
        ui::CommandId action;
        MenuOrder<ui::CommandId> order;
    };

    // Cold product/extension declarations, not a command registry or a runtime service.
    struct EditorMenuComposition final
    {
        std::vector<ui::ActionDescriptor> actions;
        std::vector<EditorMenu> menus;
        std::vector<EditorMenuGroup> groups;
        std::vector<MenuContribution> contributions;
    };

    enum class EMenuCompositionError : std::uint8_t
    {
        INVALID_ID,
        DUPLICATE_ID,
        UNKNOWN_MENU,
        UNKNOWN_GROUP,
        UNKNOWN_ACTION,
        DUPLICATE_PLACEMENT,
        UNKNOWN_ANCHOR,
        CYCLIC_ORDER
    };

    struct MenuCompositionFailure final
    {
        EMenuCompositionError code;
        std::string id;
    };

    using MenuCompositionResult = cxx::expected<ui::MenuDefinition, MenuCompositionFailure>;

    // Explicit action anchors precede group rank, priority (higher first), then canonical ID.
    // Menu/group anchors address siblings. Unknown anchors and cycles reject the whole candidate.
    [[nodiscard]] MenuCompositionResult composeEditorMenu(const EditorMenuComposition&) noexcept;
} // namespace lux::editor
