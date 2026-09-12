#include <lux/engine/editor/scene/SceneCamera.hpp>
#include <lux/engine/editor/scene/SceneViewRenderPort.hpp>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace lux::editor::workbench
{
    SceneViewRenderPort::~SceneViewRenderPort() = default;
    SceneCamera::SceneCamera() noexcept { reset(); }

    void SceneCamera::reset() noexcept
    {
        position_ = {6.0, 4.0, 8.0};
        const auto direction = (Eigen::Vector3d{0.0, 0.5, 0.0} - position_).normalized().eval();
        yaw_ = std::atan2(direction.x(), -direction.z());
        pitch_ = std::asin(direction.y());
        speed_ = 4.0;
        releaseCapture();
    }

    Eigen::Vector3d SceneCamera::forward() const noexcept
    {
        return {std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), -std::cos(yaw_) * std::cos(pitch_)};
    }

    void SceneCamera::focus(const Eigen::Vector3d& center, double radius) noexcept
    {
        if (!center.allFinite() || !std::isfinite(radius))
            return;
        position_ = center - forward() * std::max(1.0, std::abs(radius) * 2.5);
    }

    void SceneCamera::releaseCapture() noexcept { capture_ = ECapture::NONE; }
    bool SceneCamera::captured() const noexcept { return capture_ != ECapture::NONE; }

    void SceneCamera::update(
        const lux::ui::UiInputSnapshot& input, const lux::ui::ViewportResult& viewport, double seconds
    ) noexcept
    {
        const auto held = [&](lux::ui::EKey key) { return input.held[static_cast<std::size_t>(key)]; };
        const auto pressed = [&](lux::ui::EKey key) { return input.pressed[static_cast<std::size_t>(key)]; };
        if (!input.window_focused || input.modal_open || pressed(lux::ui::EKey::ESCAPE))
        {
            releaseCapture();
            return;
        }
        if (capture_ == ECapture::LOOK && !input.buttons[static_cast<std::size_t>(lux::ui::EPointerButton::RIGHT)])
            releaseCapture();
        if (capture_ == ECapture::PAN && !input.buttons[static_cast<std::size_t>(lux::ui::EPointerButton::MIDDLE)])
            releaseCapture();
        if (input.keyboard_blocked)
        {
            releaseCapture();
            return;
        }
        if (viewport.hovered && viewport.right_clicked)
            capture_ = ECapture::LOOK;
        else if (viewport.hovered && viewport.middle_clicked)
            capture_ = ECapture::PAN;
        if (viewport.window_focused && (pressed(lux::ui::EKey::HOME) || pressed(lux::ui::EKey::END)))
            reset();
        const auto right = forward().cross(Eigen::Vector3d::UnitY()).normalized().eval();
        if (capture_ == ECapture::LOOK)
        {
            yaw_ += static_cast<double>(input.pointer_delta.x) * 0.004;
            yaw_ = std::remainder(yaw_, 2.0 * std::numbers::pi);
            pitch_ = std::clamp(pitch_ - static_cast<double>(input.pointer_delta.y) * 0.004, -1.55, 1.55);
            speed_ = std::clamp(speed_ * std::exp(input.wheel.y * 0.15), 0.05, 10000.0);
            Eigen::Vector3d direction = forward() * (int(held(lux::ui::EKey::W)) - int(held(lux::ui::EKey::S))) +
                right * (int(held(lux::ui::EKey::D)) - int(held(lux::ui::EKey::A))) +
                Eigen::Vector3d::UnitY() * (int(held(lux::ui::EKey::E)) - int(held(lux::ui::EKey::Q)));
            if (direction.squaredNorm() > 0.0 && std::isfinite(seconds))
                position_ += direction.normalized() * speed_ * std::clamp(seconds, 0.0, 0.1);
        }
        else if (capture_ == ECapture::PAN)
        {
            const auto up = right.cross(forward()).eval();
            position_ += (-right * input.pointer_delta.x + up * input.pointer_delta.y) * speed_ * 0.002;
        }
        else if (viewport.hovered)
            position_ += forward() * input.wheel.y * speed_ * 0.15;
    }

    Eigen::Matrix4d SceneCamera::view(const Eigen::Vector3d& origin) const noexcept
    {
        const auto f = forward();
        const auto r = f.cross(Eigen::Vector3d::UnitY()).normalized().eval();
        const auto u = r.cross(f).eval();
        Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
        result.block<1, 3>(0, 0) = r.transpose();
        result.block<1, 3>(1, 0) = u.transpose();
        result.block<1, 3>(2, 0) = -f.transpose();
        result.block<3, 1>(0, 3) = -result.block<3, 3>(0, 0) * (position_ - origin);
        return result;
    }

    Eigen::Matrix4d SceneCamera::projection(double aspect) const noexcept
    {
        if (!std::isfinite(aspect) || aspect <= 0.0)
            aspect = 1.0;
        constexpr double near_plane = 0.05;
        constexpr double far_plane = 100000.0;
        const double scale = 1.0 / std::tan(std::numbers::pi / 6.0);
        Eigen::Matrix4d result = Eigen::Matrix4d::Zero();
        result(0, 0) = scale / aspect;
        result(1, 1) = -scale;
        result(2, 2) = far_plane / (near_plane - far_plane);
        result(2, 3) = far_plane * near_plane / (near_plane - far_plane);
        result(3, 2) = -1.0;
        return result;
    }
}
