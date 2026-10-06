#pragma once
#include <lux/engine/editor/workspace/WorkspaceValues.hpp>

namespace lux::editor::workspace
{
    enum class EDockSplit : std::uint8_t
    {
        LEAF,
        HORIZONTAL,
        VERTICAL
    };
    struct DockNode final
    {
        std::uint32_t id{};
        EDockSplit split{EDockSplit::LEAF};
        std::uint32_t first{}, second{};
        double ratio{0.5};
        std::vector<LayoutSlotId> slots;
    };
    struct DockRoot final
    {
        std::uint32_t node{};
        double x{}, y{}, width{1280}, height{720};
        bool floating{};
    };
    struct DockTree final
    {
        std::vector<DockNode> nodes;
        std::vector<DockRoot> roots;
    };
    struct LayoutSlot final
    {
        LayoutSlotId id;
        views::ViewRestoreKey restore_key;
        views::ViewTypeId type;
        bool visible{true};
        VersionedViewState state;
    };
    struct DockLayout final
    {
        std::uint32_t schema{1};
        LayoutId id;
        std::string label;
        DockTree dock;
        std::vector<LayoutSlot> slots;
        std::vector<PreservedOpaqueState> opaque;
        std::optional<LegacyOrigin> legacy_origin;
    };
    [[nodiscard]] WorkspaceResult<std::vector<std::byte>> encodeLayout(const DockLayout&, WorkspaceLimits = {});
    [[nodiscard]] WorkspaceResult<DockLayout> decodeLayout(std::span<const std::byte>, WorkspaceLimits = {});
}
