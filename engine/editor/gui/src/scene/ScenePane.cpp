#include <lux/engine/editor/gui/scene/ScenePane.hpp>
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace lux::editor::gui
{
    namespace
    {
        std::string describe(const rendering::RendererFailure &failure)
        {
            return "Renderer " + std::to_string(static_cast<unsigned>(failure.code)) + ", request " +
                   std::to_string(failure.request);
        }
    } // namespace

    ScenePane::ScenePane(scene::SceneEditor &document, rendering::EditorRenderer &renderer, std::string id)
        : DocumentPane(document, std::move(id), "Scene"), renderer_(renderer)
    {
    }

    void ScenePane::poll(PollBudget &)
    {
        if (closing_)
        {
            if (!view_)
            {
                closed_ = true;
                return;
            }
            const auto closed = view_->advanceClose();
            if (!closed)
            {
                status_ = describe(closed.error());
            }
            else if (*closed == rendering::ERenderClose::COMPLETE)
            {
                view_.reset();
                closed_ = true;
            }
            return;
        }

        if (!view_ && renderer_.state() == rendering::ERendererState::READY)
        {
            const auto scene = document_.renderScene();
            if (!scene)
            {
                status_ = scene.error().message;
                return;
            }
            auto opened = renderer_.openView(*scene, {{640, 480}, true, document_.coordinatePageSize()});
            if (!opened)
            {
                status_ = describe(opened.error());
                return;
            }
            view_ = std::move(*opened);
            updateCamera({640, 480});
        }
        if (view_ && !visible())
        {
            rotating_ = panning_ = false;
            const auto suspended = view_->requestExtent({});
            if (!suspended)
            {
                status_ = describe(suspended.error());
            }
        }
    }

    void ScenePane::updateCamera(rendering::PixelExtent extent)
    {
        if (!extent.width || !extent.height || (camera_extent_ == extent && applied_camera_revision_ == camera_revision_))
        {
            return;
        }
        const double page = document_.coordinatePageSize();
        const Eigen::Vector3d origin = (camera_.position() / page).array().floor().matrix() * page;
        const auto view = camera_.view(origin);
        const auto projection = camera_.projection(double(extent.width) / extent.height);
        if (!projection)
        {
            status_ = "Invalid camera projection";
            return;
        }
        rendering::CameraFrame frame;
        std::copy_n(view.data(), 16, frame.view.begin());
        std::copy_n(projection->data(), 16, frame.projection.begin());
        std::copy_n(origin.data(), 3, frame.origin.begin());
        frame.desired = {document_.historyId().value, 0, camera_revision_, 1};
        const auto camera = view_->setCamera(frame);
        if (!camera)
        {
            status_ = describe(camera.error());
        }
        else
        {
            camera_extent_ = extent;
            applied_camera_revision_ = camera_revision_;
        }
    }

    void ScenePane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        context.activateContext(lux::ui::UiContextIdView{id()});
        if (closing_ || !view_)
        {
            frame.textWrapped(status_.empty() ? "Preparing scene view..." : status_);
            return;
        }
        const auto view_status = view_->status();
        if (view_status.failure)
        {
            status_ = describe(*view_status.failure);
        }
        auto next = view_->acquireImage();
        if (next)
        {
            image_ = std::move(*next);
        }
        else if (next.error().code != rendering::ERendererError::NOT_READY)
        {
            status_ = describe(next.error());
        }
        if (!status_.empty())
        {
            frame.textWrapped(status_);
        }
        const auto interaction = viewport_.draw(frame, {image_.texture});
        const auto &input = ImGui::GetIO();
        const auto pixel = [](float logical, float scale)
        { return static_cast<std::uint32_t>(std::clamp(std::round(logical * scale), 0.0F, 16384.0F)); };
        const rendering::PixelExtent extent{pixel(interaction.size.width, input.DisplayFramebufferScale.x),
                                            pixel(interaction.size.height, input.DisplayFramebufferScale.y)};
        const auto resized = view_->requestExtent(extent);
        if (!resized)
        {
            status_ = describe(resized.error());
        }
        const bool blocked =
            input.WantTextInput || input.AppFocusLost || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        if (blocked)
        {
            rotating_ = panning_ = false;
        }
        else
        {
            rotating_ = (rotating_ || interaction.right_clicked) && ImGui::IsMouseDown(ImGuiMouseButton_Right);
            panning_ = (panning_ || interaction.middle_clicked) && ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        }
        CameraMotion motion;
        if (rotating_)
        {
            motion.angular_delta = {-input.MouseDelta.x * 0.004, -input.MouseDelta.y * 0.004};
        }
        if (panning_)
        {
            motion.pan_delta = {-input.MouseDelta.x * 0.015, input.MouseDelta.y * 0.015};
        }
        if (!blocked && interaction.hovered)
        {
            motion.dolly = input.MouseWheel;
        }
        if (motion.angular_delta.squaredNorm() || motion.pan_delta.squaredNorm() || motion.dolly)
        {
            const auto moved = camera_.move(motion);
            if (moved)
            {
                ++camera_revision_;
            }
        }
        updateCamera(extent);
    }

    void ScenePane::appendFrameImages(std::vector<rendering::ViewImage> &images) const
    {
        if (visible() && image_.lease.valid())
        {
            images.push_back(image_);
        }
    }

    void ScenePane::releaseFrameImages() noexcept
    {
        image_ = {};
    }

    void ScenePane::requestClose() noexcept
    {
        if (closing_)
        {
            return;
        }
        DocumentPane::requestClose();
        releaseFrameImages();
        rotating_ = panning_ = false;
        if (view_)
        {
            const auto closing = view_->beginClose();
            if (!closing)
            {
                status_ = describe(closing.error());
            }
        }
    }

    CloseStatus ScenePane::closeStatus() const
    {
        return {closed_ ? ECloseState::CLOSED : closing_ ? ECloseState::CLOSING : ECloseState::OPEN, status_};
    }
} // namespace lux::editor::gui
