#pragma once

#include <lux/engine/editor/scene/visibility.h>
#include <lux/engine/ui/UiInputEvent.hpp>
#include <lux/engine/ui/ViewportElement.hpp>
#include <Eigen/Core>

namespace lux::editor::workbench
{
    class LUX_EDITOR_SCENE_PUBLIC SceneCamera final
    {
    public:
        SceneCamera() noexcept;
        void reset() noexcept;
        void focus(const Eigen::Vector3d& center, double radius) noexcept;
        void update(const lux::ui::UiInputSnapshot &input, const lux::ui::ViewportResult &viewport,
                    double seconds) noexcept;
        void releaseCapture() noexcept;
        [[nodiscard]] bool captured() const noexcept;
        [[nodiscard]] const Eigen::Vector3d& position() const noexcept { return position_; }
        [[nodiscard]] Eigen::Vector3d forward() const noexcept;
        [[nodiscard]] Eigen::Matrix4d view(const Eigen::Vector3d& origin = Eigen::Vector3d::Zero()) const noexcept;
        [[nodiscard]] Eigen::Matrix4d projection(double aspect) const noexcept;

    private:
        Eigen::Vector3d position_{};
        double yaw_{};
        double pitch_{};
        double speed_{4.0};
        enum class ECapture { NONE, LOOK, PAN };
        ECapture capture_{ECapture::NONE};
    };
}
