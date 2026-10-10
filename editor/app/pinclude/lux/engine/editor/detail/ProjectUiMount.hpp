#pragma once
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
        void prepare(ui::Root&, std::size_t handle_count);
        void arm(std::span<const ui::PaneHandle>) noexcept;
        void disarm() noexcept;
        [[nodiscard]] std::span<const ui::PaneHandle> handles() const noexcept
        {
            return handles_;
        }

    private:
        void clear() noexcept;
        ui::Root* root_{};
        std::vector<ui::PaneHandle> handles_;
    };
} // namespace lux::editor::detail
