#pragma once
#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/editor/editing/EditHistoryTarget.hpp>
#include <memory>
namespace lux::editor::ui
{
    namespace detail
    {
        struct ActiveEditHistoryControl;
    }
    struct HistoryTargetHandle final
    {
        std::uint64_t router{}, registration{};
        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return router != 0 && registration != 0;
        }
        friend bool operator==(const HistoryTargetHandle &, const HistoryTargetHandle &) noexcept = default;
    };
    class ActiveEditHistory;
    class LUX_EDITOR_UI_PUBLIC HistoryTargetRegistration final
    {
    public:
        HistoryTargetRegistration() noexcept = default;
        ~HistoryTargetRegistration() noexcept;
        HistoryTargetRegistration(const HistoryTargetRegistration &) = delete;
        HistoryTargetRegistration &operator=(const HistoryTargetRegistration &) = delete;
        HistoryTargetRegistration(HistoryTargetRegistration &&other) noexcept;
        HistoryTargetRegistration &operator=(HistoryTargetRegistration &&other) noexcept;
        [[nodiscard]] HistoryTargetHandle handle() const noexcept;
        [[nodiscard]] editing::EditResult<void> reset() noexcept;

    private:
        friend class ActiveEditHistory;
        HistoryTargetRegistration(std::weak_ptr<detail::ActiveEditHistoryControl> control,
                                  HistoryTargetHandle handle) noexcept;
        std::weak_ptr<detail::ActiveEditHistoryControl> control_;
        HistoryTargetHandle handle_;
    };
    struct ActiveHistoryView final
    {
        HistoryTargetHandle handle;
        bool has_target{};
        editing::HistoryTargetView target;
    };
    class LUX_EDITOR_UI_PUBLIC ActiveEditHistory final
    {
    public:
        using CreateResult = editing::EditResult<std::unique_ptr<ActiveEditHistory>>;
        [[nodiscard]] static CreateResult create(std::size_t target_capacity) noexcept;
        ~ActiveEditHistory() noexcept;
        ActiveEditHistory(const ActiveEditHistory &) = delete;
        ActiveEditHistory &operator=(const ActiveEditHistory &) = delete;
        ActiveEditHistory(ActiveEditHistory &&) = delete;
        ActiveEditHistory &operator=(ActiveEditHistory &&) = delete;
        [[nodiscard]] editing::EditResult<HistoryTargetRegistration> registerTarget(
            editing::EditHistoryTarget &target) noexcept;
        // Multiple workspaces may retain one Window registration for the same target. Last reset unregisters.
        // registerTarget remains strict and rejects duplicate registration attempts.
        [[nodiscard]] editing::EditResult<HistoryTargetRegistration> retainTarget(
            editing::EditHistoryTarget &target) noexcept;
        [[nodiscard]] editing::EditResult<void> activate(HistoryTargetHandle target) noexcept;
        [[nodiscard]] editing::EditResult<void> deactivate() noexcept;
        [[nodiscard]] editing::EditResult<HistoryTargetHandle> activeTarget() const noexcept;
        [[nodiscard]] editing::EditResult<ActiveHistoryView> view() const noexcept;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> undo() noexcept;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> redo() noexcept;
        // A menu captures its registration at open time. These calls never consult or change active.
        [[nodiscard]] editing::EditResult<editing::HistoryTargetView> view(HistoryTargetHandle) const noexcept;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> undo(HistoryTargetHandle) noexcept;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> redo(HistoryTargetHandle) noexcept;
        [[nodiscard]] editing::EditResult<void> close() noexcept;

    private:
        friend class HistoryTargetRegistration;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> invoke(editing::EHistoryAction action) noexcept;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult>
        invoke(editing::EHistoryAction, HistoryTargetHandle) noexcept;
        struct Impl;
        explicit ActiveEditHistory(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] editing::EditResult<void> unregisterTarget(HistoryTargetHandle target) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::ui
