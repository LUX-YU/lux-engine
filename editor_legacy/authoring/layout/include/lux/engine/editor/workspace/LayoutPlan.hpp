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
    // Exact persistent matching input. Runtime handles and content remain with the caller's fixed snapshot.
    struct LayoutTarget final
    {
        views::ViewRestoreKey restore_key;
        views::ViewTypeId type;
    };
    struct PlannedView final
    {
        LayoutSlot slot;
        ELayoutResolution resolution{ELayoutResolution::MISSING_PROVIDER};
        // Index into the immutable input snapshot, never a fabricated or retained runtime identity.
        // Preserve the match even when its provider or schema is currently unavailable.
        std::optional<std::size_t> existing;
    };
    struct LayoutPlan final
    {
        DockLayout layout;
        std::vector<PlannedView> views;
        std::vector<LayoutTarget> retained;
    };
    class LayoutPlanner final
    {
    public:
        // These are fixed value snapshots; no factories, content binding or live UI are accepted.
        [[nodiscard]] static WorkspaceResult<LayoutPlan> resolve(
            const ValidatedLayout&,
            std::span<const LayoutTarget>,
            std::span<const ViewProviderInfo>,
            WorkspaceLimits = {}
        );
    };
}
