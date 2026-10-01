#pragma once
#include <lux/engine/editor/workspace/DockLayout.hpp>

namespace lux::editor::workspace
{
    class ValidatedLayout final
    {
    public:
        [[nodiscard]] const DockLayout& value() const noexcept
        {
            return value_;
        }
        [[nodiscard]] static WorkspaceResult<ValidatedLayout> validate(DockLayout value, WorkspaceLimits limits = {});

    private:
        explicit ValidatedLayout(DockLayout value) : value_(std::move(value)) {}
        DockLayout value_;
    };
    struct ViewProviderInfo final
    {
        views::ViewTypeId type;
        std::uint32_t first_schema{1}, last_schema{1};
    };
    enum class ELayoutResolution : std::uint8_t
    {
        REUSE,
        CREATE_UNBOUND,
        MISSING_PROVIDER,
        UNSUPPORTED_STATE
    };
    struct PlannedView final
    {
        LayoutSlot slot;
        ELayoutResolution resolution{ELayoutResolution::MISSING_PROVIDER};
        // Existing identity is preserved even when its provider or payload schema cannot currently be restored.
        std::optional<views::ViewId> existing;
    };
    struct LayoutPlan final
    {
        DockLayout layout;
        std::vector<PlannedView> views;
        std::vector<views::ViewInfo> retained;
    };
    class LayoutPlanner final
    {
    public:
        // These are fixed value snapshots; no factories, content binding or live UI are accepted.
        [[nodiscard]] static WorkspaceResult<LayoutPlan> resolve(
            const ValidatedLayout&,
            std::span<const views::ViewInfo>,
            std::span<const ViewProviderInfo>,
            WorkspaceLimits = {}
        );
    };
}
