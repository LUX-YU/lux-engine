#include <lux/engine/editor/ui/scene/SceneWorkspace.hpp>
#include <algorithm>
#include <array>
#include <cmath>
namespace lux::editor::ui
{
    struct SceneViewport::Impl final
    {
        sessions::SceneView &view;
        lux::ui::ViewportElement viewport;
        lux::ui::ViewportResult drawn;
        std::array<rendering::ViewImage, 1> images;
        std::size_t image_count{};
        bool drawn_this_frame{};
        std::optional<sessions::SceneFailure> action_failure;
        enum class ECapture : std::uint8_t
        {
            NONE,
            LOOK,
            PAN
        };
        ECapture capture{ECapture::NONE};
        double speed{4};
        explicit Impl(sessions::SceneView &source) : view(source)
        {
        }
    };
    SceneViewport::SceneViewport(lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id,
                                 sessions::SceneView &view)
        : Object(std::move(dispatcher), std::move(id), lux::ui::PaneTypeId{"lux.scene.viewport"}, "Scene View"),
          impl_(std::make_unique<Impl>(view))
    {
    }
    SceneViewport::~SceneViewport() noexcept = default;
    void SceneViewport::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        context.activateContext(lux::ui::UiContextIdView{id().name()});
        releaseFrameImages();
        impl_->drawn_this_frame = true;
        auto image = impl_->view.image();
        // Keep the same layout height while a target is being prepared. A second status row would
        // change the desired extent, alternating READY/RESIZING without any user resize.
        if (impl_->action_failure)
            frame.textMuted(impl_->action_failure->code == sessions::ESceneError::NOT_READY
                                ? "Select an object before framing it"
                                : "The view action could not be applied");
        else
            frame.textMuted(image ? "RMB + WASD/QE: fly | MMB: pan | Wheel: move | F: focus | Home: reset"
                                  : "View is preparing its image");
        if (image)
        {
            impl_->images[0] = std::move(*image);
            impl_->image_count = 1;
        }
        impl_->drawn = impl_->viewport.draw(frame, {image ? impl_->images[0].texture : lux::ui::TextureHandle{}});
    }
    std::span<const rendering::ViewImage> SceneViewport::frameImages() const noexcept
    {
        return {impl_->images.data(), impl_->image_count};
    }
    void SceneViewport::releaseFrameImages() noexcept
    {
        impl_->images[0] = {};
        impl_->image_count = 0;
    }
    void SceneViewport::cancelCapture() noexcept
    {
        impl_->capture = Impl::ECapture::NONE;
    }
    std::optional<sessions::SceneFailure> SceneViewport::actionFailure() const noexcept
    {
        return impl_->action_failure;
    }
    void SceneViewport::consumeInput(const lux::ui::UiInputSnapshot &input, double seconds,
                                     lux::ui::Vec2 scale) noexcept
    {
        const auto accept = [&](const sessions::SceneResult<void> &result, bool explicit_action = false)
        {
            if (!result)
            {
                impl_->action_failure = result.error();
                cancelCapture();
            }
            else if (explicit_action)
                impl_->action_failure.reset();
            return static_cast<bool>(result);
        };
        const bool was_drawn = std::exchange(impl_->drawn_this_frame, false);
        if (!visible() || !was_drawn)
        {
            cancelCapture();
            releaseFrameImages();
            accept(impl_->view.requestExtent({}));
            return;
        }
        const auto extent = impl_->drawn.size;
        if (std::isfinite(extent.width) && std::isfinite(extent.height) && scale.x > 0 && scale.y > 0)
        {
            const auto width = std::clamp(std::round(extent.width * scale.x), 0.0F, 16384.0F);
            const auto height = std::clamp(std::round(extent.height * scale.y), 0.0F, 16384.0F);
            if (!accept(
                    impl_->view.requestExtent({static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)})))
                return;
        }
        const auto held = [&](lux::ui::EKey key) { return input.held[static_cast<std::size_t>(key)]; };
        const auto pressed = [&](lux::ui::EKey key) { return input.pressed[static_cast<std::size_t>(key)]; };
        if (!input.window_focused || input.keyboard_blocked || input.modal_open || pressed(lux::ui::EKey::ESCAPE))
        {
            cancelCapture();
            return;
        }
        if (impl_->capture == Impl::ECapture::LOOK &&
            !input.buttons[static_cast<std::size_t>(lux::ui::EPointerButton::RIGHT)])
            cancelCapture();
        if (impl_->capture == Impl::ECapture::PAN &&
            !input.buttons[static_cast<std::size_t>(lux::ui::EPointerButton::MIDDLE)])
            cancelCapture();
        if (impl_->drawn.hovered && impl_->drawn.right_clicked)
            impl_->capture = Impl::ECapture::LOOK;
        else if (impl_->drawn.hovered && impl_->drawn.middle_clicked)
            impl_->capture = Impl::ECapture::PAN;
        if (impl_->drawn.window_focused && pressed(lux::ui::EKey::F))
            if (!accept(impl_->view.frameSelection(), true))
                return;
        if (impl_->drawn.window_focused && pressed(lux::ui::EKey::HOME))
            if (!accept(impl_->view.resetCamera(), true))
                return;
        sessions::CameraMotion motion;
        if (impl_->capture == Impl::ECapture::LOOK)
        {
            motion.angular_delta = {input.pointer_delta.x * 0.004, -input.pointer_delta.y * 0.004};
            impl_->speed = std::clamp(impl_->speed * std::exp(input.wheel.y * 0.15), 0.05, 10000.0);
            Eigen::Vector3d direction{double(held(lux::ui::EKey::D)) - double(held(lux::ui::EKey::A)),
                                      double(held(lux::ui::EKey::E)) - double(held(lux::ui::EKey::Q)),
                                      double(held(lux::ui::EKey::W)) - double(held(lux::ui::EKey::S))};
            if (direction.squaredNorm() && std::isfinite(seconds))
                motion.local_translation = direction.normalized() * impl_->speed * std::clamp(seconds, 0.0, 0.1);
        }
        else if (impl_->capture == Impl::ECapture::PAN)
            motion.pan_delta = Eigen::Vector2d{-input.pointer_delta.x, input.pointer_delta.y} * impl_->speed * 0.002;
        else if (impl_->drawn.hovered)
            motion.dolly = input.wheel.y * impl_->speed * 0.15;
        const bool moving = motion.local_translation.squaredNorm() || motion.angular_delta.squaredNorm() ||
                            motion.pan_delta.squaredNorm() || motion.dolly;
        if (moving)
            accept(impl_->view.moveCamera(motion), true);
    }
} // namespace lux::editor::ui
