#include <lux/engine/editor/scene/SceneCamera.hpp>
#include <cassert>
#include <cmath>

int main()
{
    lux::editor::workbench::SceneCamera camera;
    const auto initial = camera.position();
    lux::ui::UiInputSnapshot input;
    input.window_focused = true;
    input.held[static_cast<std::size_t>(lux::ui::EKey::W)] = true;
    lux::ui::ViewportResult viewport;
    camera.update(input, viewport, 0.1);
    assert(camera.position() == initial);
    viewport.hovered = viewport.right_clicked = viewport.window_focused = true;
    input.buttons[static_cast<std::size_t>(lux::ui::EPointerButton::RIGHT)] = true;
    camera.update(input, viewport, 0.1);
    assert(camera.captured());
    assert((camera.position() - initial).norm() > 0.0);
    viewport.hovered = viewport.right_clicked = false;
    const auto flying = camera.position();
    camera.update(input, viewport, 0.1);
    assert((camera.position() - flying).norm() > 0.0);
    input.keyboard_blocked = true;
    const auto blocked = camera.position();
    camera.update(input, viewport, 0.1);
    assert(!camera.captured() && camera.position() == blocked);
    const auto projection = camera.projection(16.0 / 9.0);
    for (const double distance : {0.05, 100000.0})
    {
        const Eigen::Vector4d clip = projection * Eigen::Vector4d{0.0, 0.0, -distance, 1.0};
        assert(std::abs(clip.z() / clip.w() - (distance == 0.05 ? 0.0 : 1.0)) < 1e-8);
    }
    camera.focus({1e12, 0.0, 1e12}, 5.0);
    assert((camera.view(camera.position()).block<3, 1>(0, 3).norm() == 0.0));
}
