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

    rendering::RenderView *ScenePane::view() noexcept
    {
        if (auto *author = std::get_if<std::unique_ptr<rendering::RenderView>>(&view_owner_))
        {
            return author->get();
        }
        if (auto *run = std::get_if<scene::RunViewLease>(&view_owner_))
        {
            return &run->view();
        }
        return nullptr;
    }

    void ScenePane::remember(std::string_view operation, const rendering::RendererFailure &failure)
    {
        view_result_ =
            lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, std::string(operation),
                                               static_cast<std::uint64_t>(failure.code), describe(failure), failure});
        status_ = view_result_.error().message;
    }

    void ScenePane::poll(PollBudget &budget)
    {
        DocumentPane::poll(budget);
        const auto run = document_.runStatus();
        const bool running = run.state == scene::ERunState::RUNNING || run.state == scene::ERunState::PAUSED;
        const bool author = run.state == scene::ERunState::IDLE || run.state == scene::ERunState::FINISHED ||
                            run.state == scene::ERunState::FAILED;
        const auto wanted = running ? run.id : scene::RunId{};
        const bool switching = displayed_run_ != wanted || (!running && !author);
        if (!view() && (reopen_ || switching))
        {
            displayed_run_ = wanted;
            view_result_ = {};
            reopen_ = false;
        }
        if (auto *current = view(); current && (closing_ || switching || reopen_))
        {
            releaseFrameImages();
            rotating_ = panning_ = false;
            auto requested = current->beginClose();
            if (!requested)
            {
                remember("scene.view.beginClose", requested.error());
                return;
            }
            auto closed = current->advanceClose();
            if (!closed)
            {
                remember("scene.view.advanceClose", closed.error());
                return;
            }
            if (*closed != rendering::ERenderClose::COMPLETE)
            {
                return;
            }
            view_owner_.emplace<std::monostate>();
            displayed_run_ = {};
            view_result_ = {};
            status_.clear();
            camera_extent_ = {};
            applied_camera_revision_ = 0;
            reopen_ = false;
        }
        if (closing_)
        {
            closed_ = !view();
            return;
        }
        if (!view() && visible() && (running || author) && renderer_.state() == rendering::ERendererState::READY &&
            view_result_)
        {
            if (running)
            {
                if (!run.render_scene.isValid())
                {
                    status_ = "This Scene runs without a RenderSystem";
                    return;
                }
                auto opened = document_.openRunView(run.id, {{640, 480}, true, document_.runCoordinatePageSize()});
                if (!opened)
                {
                    view_result_ = lux::cxx::unexpected(opened.error());
                    status_ = opened.error().message;
                    return;
                }
                view_owner_.emplace<scene::RunViewLease>(std::move(*opened));
                displayed_run_ = run.id;
            }
            else
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
                    remember("scene.view.open", opened.error());
                    return;
                }
                view_owner_.emplace<std::unique_ptr<rendering::RenderView>>(std::move(*opened));
            }
            updateCamera({640, 480});
        }
        if (auto *current = view(); current && !visible())
        {
            rotating_ = panning_ = false;
            const auto suspended = current->requestExtent({});
            if (!suspended)
            {
                remember("scene.view.suspend", suspended.error());
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
        const double page = displayed_run_.serial ? document_.runCoordinatePageSize() : document_.coordinatePageSize();
        const Eigen::Vector3d origin = (camera_.position() / page).array().floor().matrix() * page;
        const auto camera_view = camera_.view(origin);
        const auto projection = camera_.projection(double(extent.width) / extent.height);
        if (!projection)
        {
            status_ = "Invalid camera projection";
            return;
        }
        rendering::CameraFrame frame;
        std::copy_n(camera_view.data(), 16, frame.view.begin());
        std::copy_n(projection->data(), 16, frame.projection.begin());
        std::copy_n(origin.data(), 3, frame.origin.begin());
        frame.desired = {document_.historyId().value, 0, camera_revision_, 1};
        const auto camera = view()->setCamera(frame);
        if (!camera)
        {
            remember("scene.view.camera", camera.error());
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
        const auto run = document_.runStatus();
        if (run.state == scene::ERunState::IDLE || run.state == scene::ERunState::FINISHED ||
            run.state == scene::ERunState::FAILED)
        {
            if (ImGui::Button("Play"))
            {
                auto started = document_.play();
                if (!started)
                {
                    status_ = started.error().domain + ": " + started.error().message;
                }
            }
        }
        else
        {
            const auto action = [&](EditorResult<void> result)
            {
                if (!result)
                {
                    status_ = result.error().domain + ": " + result.error().message;
                }
            };
            if (run.state == scene::ERunState::RUNNING && !run.pause_pending && ImGui::Button("Pause"))
            {
                action(document_.pauseRun(run.id));
            }
            if (run.state == scene::ERunState::PAUSED)
            {
                if (ImGui::Button("Resume"))
                {
                    action(document_.resumeRun(run.id));
                }
                ImGui::SameLine();
                if (ImGui::Button("Step"))
                {
                    action(document_.stepRun(run.id));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
            {
                action(document_.stopRun(run.id));
            }
            ImGui::SameLine();
            ImGui::Text("Step %llu | %.3f s", static_cast<unsigned long long>(run.steps),
                        std::chrono::duration<double>(run.elapsed).count());
        }
        if (!run.result)
        {
            frame.textWrapped(run.result.error().domain + ": " + run.result.error().message);
        }
        if (closing_ || !view())
        {
            frame.textWrapped(status_.empty() ? "Preparing scene view..." : status_);
            if (!closing_ && !view_result_ && ImGui::SmallButton("Reopen view"))
            {
                reopen_ = true;
            }
            return;
        }
        const auto view_status = view()->status();
        if (view_status.failure)
        {
            remember("scene.view.status", *view_status.failure);
        }
        auto next = view()->acquireImage();
        if (next)
        {
            image_ = std::move(*next);
        }
        else if (next.error().code != rendering::ERendererError::NOT_READY)
        {
            remember("scene.view.image", next.error());
        }
        // Keep readiness/error feedback on one fixed-height line so it cannot
        // create a resize -> NOT_READY -> layout resize feedback loop.
        if (!view_result_)
        {
            if (ImGui::SmallButton("Reopen view"))
            {
                reopen_ = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(view_result_.error().message.c_str());
        }
        else if (!status_.empty())
        {
            ImGui::TextUnformatted(status_.c_str());
        }
        else
        {
            ImGui::Dummy({0, ImGui::GetFrameHeight()});
        }
        if (!displayed_run_.serial)
        {
            drawPlacement(frame);
        }
        const auto interaction = viewport_.draw(frame, {image_.texture});
        if (!displayed_run_.serial && ImGui::BeginDragDropTarget())
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
        if (view_status.state != rendering::EViewState::FAILED)
        {
            const auto resized = view()->requestExtent(extent);
            if (!resized)
            {
                remember("scene.view.resize", resized.error());
            }
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
        // Visibility can change during draw; the already recorded image still
        // belongs to this frame until seal/releaseFrameImages completes.
        if (image_.lease.valid())
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
