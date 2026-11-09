#pragma once
#include <lux/engine/editor/EditorLayout.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/ui/Docking.hpp>
#include <lux/engine/ui/PaneHandle.hpp>
#include <span>
#include <vector>

namespace lux::ui
{
    class Root;
}

namespace lux::editor::detail
{
    // Owns only responsibility to unmount these registrations. Root owns every Pane.
    class ProjectUiMount final
    {
    public:
        ProjectUiMount() noexcept = default;
        ~ProjectUiMount() noexcept;
        ProjectUiMount(ProjectUiMount&&) noexcept;
        ProjectUiMount& operator=(ProjectUiMount&&) noexcept;
        ProjectUiMount(const ProjectUiMount&) = delete;
        ProjectUiMount& operator=(const ProjectUiMount&) = delete;
        [[nodiscard]] static ProjectResult<ui::PanePlacement> placement(const ProjectLayout&) noexcept;
        void prepare(ui::Root&, std::size_t handle_count);
        void arm(std::span<const ui::PaneHandle>) noexcept;
        void disarm() noexcept;
        [[nodiscard]] FrameworkResult<void> unmount() noexcept;

        [[nodiscard]] std::span<const ui::PaneHandle> handles() const noexcept
        {
            return handles_;
        }

    private:
        ui::Root* root_{};
        std::vector<ui::PaneHandle> handles_;
    };
} // namespace lux::editor::detail
