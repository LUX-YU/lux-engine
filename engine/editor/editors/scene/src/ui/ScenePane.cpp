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

    ScenePane::ScenePane(scene::SceneEditor &document, rendering::EditorRenderer &renderer, std::string id,
                         std::span<const SpatialViewportRegistration> registrations)
        : DocumentPane(document, std::move(id), "Scene"), renderer_(renderer)
    {
        const std::array builtin{spatialViewport3D()};
        if (registrations.empty())
        {
            registrations = builtin;
        }
        for (const auto &registration : registrations)
        {
            if (registration.supports && registration.create && registration.supports(document))
            {
                if (spatial_)
                {
                    spatial_.reset();
                    status_ = "Multiple spatial viewport providers match this World";
                    return;
                }
                spatial_ = registration.create();
            }
        }
        if (!spatial_)
        {
            status_ = "No spatial viewport supports this World's declared space";
        }
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
            document_.unbindCamera(camera_, current->handle());
            camera_ = {};
            navigation_pending_ = false;
            action_.emplace<std::monostate>();
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
            reopen_ = false;
        }
        if (closing_)
        {
            closed_ = !view();
            return;
        }
        if (spatial_ && !view() && visible() && (running || author) &&
            renderer_.state() == rendering::ERendererState::READY && view_result_)
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
        }
        if (view() && !switching)
        {
            if (!displayed_run_.serial)
            {
                const auto plane = document_.setWorkPlaneHeight(work_plane_height_);
                if (!plane)
                {
                    status_ = plane.error().message;
                }
            }
            if (navigation_pending_ && spatial_ && camera_.valid())
            {
                const auto moved = spatial_->navigate(document_, camera_, pending_motion_);
                navigation_pending_ = false;
                pending_motion_ = {};
                if (!moved)
                {
                    status_ = moved.error().message;
                }
            }
            updateCamera(view()->status().ready_extent);
            if (action_.index() != 0 && spatial_)
            {
                const Pick location =
                    action_.index() == 1 ? std::get<Pick>(action_) : std::get<Create>(action_).location;
                bool completed = true;
                if (location.instance != document_.instance())
                {
                    status_ = "The action belongs to a previous Scene instance";
                }
                else if (!document_.component(location.camera, lux::cxx::typeToken<lux::scene::Camera>()))
                {
                    status_ = "The action's camera no longer exists";
                }
                else if (action_.index() == 1 && document_.selection().revision != location.selection_revision)
                {
                    status_ = "A newer selection superseded the pending pick";
                }
                else if (const auto *create = std::get_if<Create>(&action_))
                {
                    const auto history = document_.historyView();
                    if (!history || history->history.current != create->base)
                    {
                        status_ = "Content changed after the model drop; repeat the operation";
                        action_.emplace<std::monostate>();
                        return;
                    }
                    const auto point =
                        spatial_->creationPoint(document_, location.instance, location.ray, create->plane);
                    if (!point)
                    {
                        status_ = point.error().message;
                        const auto *query = std::any_cast<lux::scene::MeshQueryFailure>(&point.error().cause);
                        completed = !query || query->code != lux::scene::EMeshQueryError::NOT_READY;
                    }
                    else
                    {
                        const auto accepted = document_.requestModelCreation(create->asset, *point, create->partition);
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
                else
                {
                    lux::scene::RayHit3D hit;
                    const auto picked = document_.raycastNearest(location.instance, location.ray, 1.0e12, hit);
                    if (!picked)
                    {
                        completed = picked.error().code != lux::scene::EMeshQueryError::NOT_READY;
                        status_ = "Mesh query " + std::to_string(static_cast<unsigned>(picked.error().code));
                    }
                    else
                    {
                        const auto selected = document_.select(
                            {location.instance, *picked ? hit.entity : lux::simulation::ecs::NullEntity});
                        if (!selected)
                        {
                            status_ = selected.error().message;
                        }
                        else
                        {
                            status_.clear();
                        }
                    }
                }
                if (completed)
                {
                    action_.emplace<std::monostate>();
                }
            }
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
        if (!view() || view()->handle().isNull() || !extent.width || !extent.height)
        {
            return;
        }
        auto wanted = document_.viewportCamera();
        if (!wanted || *wanted != camera_)
        {
            document_.unbindCamera(camera_, view()->handle());
            camera_ = {};
        }
        bool enabled{};
        if (wanted)
        {
            const auto bound = document_.bindCamera(*wanted, view()->handle(), double(extent.width) / extent.height);
            if (bound)
            {
                camera_ = *wanted;
                enabled = document_.component(camera_, lux::cxx::typeToken<lux::simulation::ecs::WorldTransform3D>()) !=
                          nullptr;
            }
            else
            {
                status_ = bound.error().message;
            }
        }
        else
        {
            status_ = wanted.error().message;
        }
        const auto history = document_.historyView();
        const auto output =
            view()->setOutput({document_.instance().value, history ? history->history.revision.value : 0,
                               document_.componentVersion(camera_, lux::cxx::typeToken<lux::scene::Camera>()), 0},
                              enabled);
        if (!output)
        {
            remember("scene.view.output", output.error());
        }
        camera_extent_ = extent;
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
        ImGui::SetNextItemWidth(100.0F);
        ImGui::DragScalar("Work plane Y", ImGuiDataType_Double, &work_plane_height_, 0.1F);
        if (!placement_.serial)
        {
            return;
        }
        auto state = document_.modelCreationStatus(placement_);
        if (!state)
        {
            status_ = state.error().domain + ": " + state.error().message;
            placement_ = {};
            return;
        }
        if (std::holds_alternative<scene::ModelCreationPending>(*state))
        {
            frame.textMuted("Loading model...");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel placement"))
            {
                static_cast<void>(document_.cancelModelCreation(placement_));
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
                    auto retried = document_.retryModelCreation(placement_, history->history.current);
                    if (!retried)
                    {
                        status_ = retried.error().domain + ": " + retried.error().message;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Discard placement") && document_.cancelModelCreation(placement_) &&
                document_.acknowledgeModelCreation(placement_))
            {
                placement_ = {};
            }
        }
        else if (document_.acknowledgeModelCreation(placement_))
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
        if (!displayed_run_.serial && camera_.valid())
        {
            const auto *camera = static_cast<const lux::scene::Camera *>(
                document_.component(camera_, lux::cxx::typeToken<lux::scene::Camera>()));
            const auto *pose = static_cast<const lux::simulation::ecs::Transform3D *>(
                document_.component(camera_, lux::cxx::typeToken<lux::simulation::ecs::Transform3D>()));
            if (camera && pose)
            {
                ImGui::SameLine();
                int projection = static_cast<int>(camera->projection.index());
                ImGui::SetNextItemWidth(140.0F);
                if (ImGui::Combo("##projection", &projection, "Perspective\0Orthographic\0"))
                {
                    auto next = *camera;
                    if (projection == 0)
                    {
                        next.projection = lux::scene::PerspectiveProjection{};
                    }
                    else
                    {
                        next.projection = lux::scene::OrthographicProjection{};
                    }
                    const auto changed = document_.navigateCamera(camera_, *pose, next);
                    if (!changed)
                    {
                        status_ = changed.error().message;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Create game camera"))
                {
                    const auto history = document_.historyView();
                    if (history)
                    {
                        const auto created =
                            document_.createCameraFromView(camera_, history->history.current, {partition_});
                        if (!created)
                        {
                            status_ = created.error().message.data();
                        }
                    }
                }
            }
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
                if (!reference)
                {
                    status_ = reference.error().domain + ": " + reference.error().message;
                }
                else if (spatial_)
                {
                    const auto ray =
                        spatial_->ray(document_, camera_, {interaction.local_pointer.x, interaction.local_pointer.y},
                                      {interaction.size.width, interaction.size.height});
                    if (!ray)
                    {
                        status_ = ray.error().message;
                    }
                    else
                    {
                        action_.emplace<Create>(
                            Pick{document_.instance(), *ray, camera_, document_.selection().revision}, *reference,
                            lux::partition::PartitionOrdinal{partition_}, work_plane_height_,
                            document_.historyView()->history.current);
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
        if (!displayed_run_.serial &&
            (motion.angular_delta.squaredNorm() || motion.pan_delta.squaredNorm() || motion.dolly))
        {
            pending_motion_.angular_delta += motion.angular_delta;
            pending_motion_.pan_delta += motion.pan_delta;
            pending_motion_.dolly += motion.dolly;
            navigation_pending_ = true;
        }
        if (spatial_ && interaction.left_clicked && !blocked && !rotating_ && !panning_ && !ImGui::GetDragDropPayload())
        {
            const auto ray =
                spatial_->ray(document_, camera_, {interaction.local_pointer.x, interaction.local_pointer.y},
                              {interaction.size.width, interaction.size.height});
            if (ray)
            {
                action_.emplace<Pick>(document_.instance(), *ray, camera_, document_.selection().revision);
            }
            else
            {
                status_ = ray.error().message;
            }
        }
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
