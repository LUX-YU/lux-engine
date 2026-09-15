#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <lux/engine/editor/gui/asset/AssetReference.hpp>
#include <lux/engine/editor/gui/scene/ScenePane.hpp>

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

    void ScenePane::poll(PollBudget &budget)
    {
        DocumentPane::poll(budget);
        if (closing_)
        {
            releaseFrameImages();
            rotating_ = panning_ = false;
            if (!view_)
            {
                closed_ = true;
                return;
            }
            const auto requested = view_->beginClose();
            if (!requested)
            {
                status_ = describe(requested.error());
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
        if (!extent.width || !extent.height ||
            (camera_extent_ == extent && applied_camera_revision_ == camera_revision_))
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

    void ScenePane::drawPlacement(lux::ui::Frame &frame)
    {
        if (document_.partitionCount() > 1)
        {
            const auto label = std::to_string(partition_);
            if (ImGui::BeginCombo("Partition", label.c_str()))
            {
                for (std::size_t index{}; index < document_.partitionCount(); ++index)
                {
                    if (ImGui::Selectable(std::to_string(index).c_str(), index == partition_))
                    {
                        partition_ = static_cast<std::uint32_t>(index);
                    }
                }
                ImGui::EndCombo();
            }
        }
        if (!placement_.serial)
        {
            return;
        }
        auto state = document_.modelPlacementStatus(placement_);
        if (!state)
        {
            status_ = state.error().domain + ": " + state.error().message;
            placement_ = {};
            return;
        }
        if (std::holds_alternative<scene::ModelPlacementPending>(*state))
        {
            frame.textMuted("Loading model...");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel placement"))
            {
                static_cast<void>(document_.cancelModelPlacement(placement_));
            }
        }
        else if (const auto *failure = std::get_if<EditorFailure>(&*state))
        {
            frame.textWrapped(failure->domain + ": " + std::to_string(failure->reason) + " " + failure->message);
            if (ImGui::SmallButton("Retry placement"))
            {
                const auto history = document_.historyView();
                if (history)
                {
                    auto retried = document_.retryModelPlacement(placement_, history->history.current);
                    if (!retried)
                    {
                        status_ = retried.error().domain + ": " + retried.error().message;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Discard placement") && document_.cancelModelPlacement(placement_) &&
                document_.acknowledgeModelPlacement(placement_))
            {
                placement_ = {};
            }
        }
        else if (document_.acknowledgeModelPlacement(placement_))
        {
            placement_ = {};
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
        drawPlacement(frame);
        const auto interaction = viewport_.draw(frame, {image_.texture});
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto *payload = ImGui::AcceptDragDropPayload(kAssetReferencePayload))
            {
                const auto reference = decodeAssetReference(
                    {static_cast<const std::byte *>(payload->Data), static_cast<std::size_t>(payload->DataSize)});
                const auto projection = camera_.projection(interaction.size.width / interaction.size.height);
                if (!reference)
                {
                    status_ = reference.error().domain + ": " + reference.error().message;
                }
                else if (projection)
                {
                    const Eigen::Vector4d screen{2.0 * interaction.local_pointer.x / interaction.size.width - 1.0,
                                                 2.0 * interaction.local_pointer.y / interaction.size.height - 1.0, 0.0,
                                                 1.0};
                    const Eigen::Vector4d ray = ((*projection) * camera_.view(camera_.position())).inverse() * screen;
                    const Eigen::Vector3d direction = (ray.head<3>() / ray.w()).normalized();
                    const auto distance =
                        std::abs(direction.y()) > 1e-8 ? -camera_.position().y() / direction.y() : -1.0;
                    const Eigen::Vector3d position = camera_.position() + direction * (distance > 0 ? distance : 5.0);
                    auto accepted = document_.requestModelPlacement(*reference, position,
                                                                    lux::partition::PartitionOrdinal{partition_});
                    if (accepted)
                    {
                        placement_ = *accepted;
                        status_.clear();
                    }
                    else
                    {
                        status_ = accepted.error().domain + ": " + accepted.error().message;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
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
        DocumentPane::requestClose();
    }

    CloseStatus ScenePane::closeStatus() const
    {
        return {closed_ ? ECloseState::CLOSED : closing_ ? ECloseState::CLOSING : ECloseState::OPEN, status_};
    }
} // namespace lux::editor::gui
