#pragma once

#include <lux/engine/editor/metadata/PaneRegistration.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <span>
#include <vector>

namespace lux::editor
{
    class EditorContext;

    // Owns only complete top-level windows. Factories decide whether to reuse a window.
    class PaneManager final
    {
    public:
        PaneManager(lux::ui::Root&, EditorContext&) noexcept;
        ~PaneManager();
        PaneManager(const PaneManager&) = delete;
        PaneManager& operator=(const PaneManager&) = delete;

        [[nodiscard]] EditorResult<void> setRegistrations(std::vector<PaneRegistration>);
        [[nodiscard]] std::span<const PaneRegistration> registrations() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] std::span<const std::unique_ptr<lux::ui::Pane>> panes() const noexcept;
        [[nodiscard]] PaneRegistration::CreateResult create(lux::ui::PaneTypeIdView) noexcept;
        [[nodiscard]] PaneRegistration::CreateResult adopt(std::unique_ptr<lux::ui::Pane>) noexcept;
        [[nodiscard]] lux::ui::Pane* find(lux::ui::PaneIdView) const noexcept;
        [[nodiscard]] lux::ui::Pane* findFirst(lux::ui::PaneTypeIdView) const noexcept;
        [[nodiscard]] lux::ui::PaneId makeId();
        [[nodiscard]] bool erase(lux::ui::PaneIdView) noexcept;
        void setFrozen(bool) noexcept;
        [[nodiscard]] bool frozen() const noexcept;
        void show(lux::ui::Pane&) noexcept;
        [[nodiscard]] lux::ui::Root& root() const noexcept;
        [[nodiscard]] EditorContext& context() const noexcept;

    private:
        lux::ui::Root& root_;
        EditorContext& context_;
        std::uint64_t revision_{};
        bool frozen_{};
        // Retain replaced implementations until all windows have been destroyed.
        std::vector<std::shared_ptr<const void>> retained_code_;
        std::vector<PaneRegistration> registrations_;
        std::vector<std::unique_ptr<lux::ui::Pane>> panes_;
    };
}
