#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <lux/engine/editor/ui/asset/AssetDragDrop.hpp>
#include <lux/engine/editor/ui/scene/SceneContentElement.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/RenderAssets.hpp>

namespace lux::editor::ui
{
    namespace
    {
        std::string describe(const lux::render::RendererFailure& failure)
        {
            return "Renderer " + std::to_string(static_cast<unsigned>(failure.code)) + ", request " +
                   std::to_string(failure.request);
        }
    } // namespace

    SceneContentElement::SceneContentElement(
        scene::SceneEditor::Impl& editor,
        lux::scene::SceneRuntime& runtime,
        const std::shared_ptr<const lux::scene::ScenePackage>& source,
        lux::scene::RenderResources& resources,
        std::string id,
        EditorResult<void>& status,
        std::span<const SpatialInteractionRegistration> registrations
    )
        : lux::ui::Element(*editor.editor, lux::ui::ElementId{std::move(id)}),
          layout_(*this, lux::ui::ElementId{"layout"}), assets_(layout_, *editor.editor, editor.project(), status),
          toolbar_(layout_, lux::ui::ElementId{"playback"}, lux::ui::ELayoutType::HORIZONTAL),
          play_(toolbar_, lux::ui::ElementId{"play"}, "Play"), pause_(toolbar_, lux::ui::ElementId{"pause"}, "Pause"),
          resume_(toolbar_, lux::ui::ElementId{"resume"}, "Resume"),
          step_(toolbar_, lux::ui::ElementId{"step"}, "Step"), stop_(toolbar_, lux::ui::ElementId{"stop"}, "Stop"),
          clock_(toolbar_, lux::ui::ElementId{"clock"}), details_(*this), editor_(editor), runtime_(runtime),
          source_(source), resources_(resources)
    {
        toolbar_.setStretch({1, 0});
        const auto connect = [this, &status](lux::ui::Button& button, EControl action) {
            return lux::editor::detail::takeConnection(
                lux::object::LuxObject::connect(
                    std::addressof(button),
                    &lux::ui::Button::activated,
                    [this, action]() noexcept {
                        control_ = action;
                        control_run_ = editor_.runStatus().id;
                    }
                ),
                status
            );
        };
        controls_[0] = connect(play_, EControl::PLAY);
        controls_[1] = connect(pause_, EControl::PAUSE);
        controls_[2] = connect(resume_, EControl::RESUME);
        controls_[3] = connect(step_, EControl::STEP);
        controls_[4] = connect(stop_, EControl::STOP);
        // The builtin camera controller is selected explicitly. It is enabled only
        // when the current scene exposes the corresponding transform/camera data.
        const auto builtin = spatialInteraction3D();
        spatial_ = builtin.create();
    }

    lux::scene::RenderResourceId SceneContentElement::view() noexcept
    {
        return viewport_ ? viewport_->view() : lux::scene::RenderResourceId{};
    }

    EditorResult<lux::math::Ray3d> SceneContentElement::cameraRay(Eigen::Vector2d point, Eigen::Vector2d extent) const
    {
        if (camera_scene_ != editor_.instance())
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "viewport.camera"});
        const auto* pose = static_cast<const lux::simulation::ecs::WorldTransform3D*>(
            editor_.component(camera_, lux::cxx::typeToken<lux::simulation::ecs::WorldTransform3D>())
        );
        const auto* camera =
            static_cast<const lux::scene::Camera*>(editor_.component(camera_, lux::cxx::typeToken<lux::scene::Camera>())
            );
        if (!pose || !camera)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "viewport.camera"});
        return spatial_->ray(*pose, *camera, point, extent);
    }

    void SceneContentElement::remember(std::string_view operation, const lux::render::RendererFailure& failure)
    {
        view_result_ = lux::cxx::unexpected(EditorFailure{
            EEditorError::FRONTEND_FAILURE,
            std::string(operation),
            static_cast<std::uint64_t>(failure.code),
            describe(failure),
            failure
        });
        status_ = view_result_.error().message;
    }

    void SceneContentElement::applyViewChange() noexcept
    {
        const auto run = editor_.runStatus();
        const bool running = run.state == scene::ERunState::RUNNING || run.state == scene::ERunState::PAUSED;
        const bool author = run.state == scene::ERunState::IDLE || run.state == scene::ERunState::FINISHED ||
                            run.state == scene::ERunState::FAILED;
        const auto wanted = running ? run.id : scene::RunId{};
        const bool switching =
            displayed_run_ != wanted || (!running && !author) || displayed_render_ != editor_.selectedRenderSystem();
        if (!viewport_ && (reopen_ || switching))
        {
            displayed_run_ = wanted;
            displayed_render_ = editor_.selectedRenderSystem();
            view_result_ = {};
            reopen_ = false;
        }
        if (viewport_ && (closing_ || switching || reopen_))
        {
            rotating_ = panning_ = false;
            navigation_pending_ = false;
            action_.emplace<std::monostate>();
            if (viewport_->close() != lux::render::ERenderClose::COMPLETE)
                return;
            viewport_.reset();
            camera_ = lux::simulation::ecs::NullEntity;
            camera_scene_ = {};
            displayed_run_ = {};
            view_result_ = {};
            status_.clear();
            reopen_ = false;
        }
        if (closing_)
        {
            closed_ = !viewport_;
            return;
        }
        if (editor_.instance().valid() && spatial_ && !viewport_ && visible() && (running || author) && view_result_)
        {
            auto camera = editor_.viewportCamera();
            const auto instance = editor_.instance();
            const auto render_system = editor_.selectedRenderSystem();
            if (!camera || !instance.valid() || !render_system.valid())
            {
                status_ = camera ? "Scene is unavailable" : camera.error().message;
                return;
            }
            auto opened = ui::SceneElement::create(
                layout_,
                lux::ui::ElementId{std::string(id().name()) + ".view"},
                runtime_,
                instance,
                resources_,
                render_system,
                *camera,
                {{640, 480}, lux::scene::SampledOutput{}}
            );
            if (!opened)
            {
                if (opened.error().code != lux::render::ERendererError::BUSY)
                    remember("scene.view.create", opened.error());
                return;
            }
            viewport_ = std::move(*opened);
            camera_ = *camera;
            camera_scene_ = instance;
            displayed_run_ = wanted;
            displayed_render_ = editor_.selectedRenderSystem();
        }
    }

    void SceneContentElement::update() noexcept
    {
        if (control_ != EControl::NONE || render_selection_ || detail_action_)
            root().deferChange(*this, [](object::LuxObject& target) noexcept {
                static_cast<SceneContentElement&>(target).applyControl();
            });
        if (!closing_ && placement_.serial)
        {
            const auto status = editor_.modelCreationStatus(placement_);
            const bool completed = status && (std::holds_alternative<scene::ModelCreationSucceeded>(*status) ||
                                              std::holds_alternative<scene::ModelCreationCancelled>(*status));
            if (completed && editor_.acknowledgeModelCreation(placement_))
                placement_ = {};
        }
        const auto playback = editor_.runStatus();
        const bool idle = playback.state == scene::ERunState::IDLE || playback.state == scene::ERunState::FINISHED ||
                          playback.state == scene::ERunState::FAILED;
        play_.setVisible(idle);
        play_.setEnabled(editor_.instance().valid() && editor_.asset_status_.phase == EAssetEditPhase::IDLE);
        pause_.setVisible(playback.state == scene::ERunState::RUNNING);
        pause_.setEnabled(!playback.pause_pending);
        resume_.setVisible(playback.state == scene::ERunState::PAUSED);
        step_.setVisible(playback.state == scene::ERunState::PAUSED);
        stop_.setVisible(!idle);
        clock_.setText("Step " + std::to_string(playback.steps));

        const auto run = editor_.runStatus();
        const bool running = run.state == scene::ERunState::RUNNING || run.state == scene::ERunState::PAUSED;
        const bool author = run.state == scene::ERunState::IDLE || run.state == scene::ERunState::FINISHED ||
                            run.state == scene::ERunState::FAILED;
        const auto wanted = running ? run.id : scene::RunId{};
        const bool switching =
            displayed_run_ != wanted || (!running && !author) || displayed_render_ != editor_.selectedRenderSystem();
        const bool needs_view = !viewport_ && editor_.instance().valid() && visible() && (running || author);
        if (needs_view || closing_ || switching || reopen_)
            root().deferChange(*this, [](object::LuxObject& target) noexcept {
                static_cast<SceneContentElement&>(target).applyViewChange();
            });
        if (closing_)
            return;

        if (view().isValid() && !switching)
        {
            if (!displayed_run_.serial)
            {
                const auto plane = editor_.setWorkPlaneHeight(work_plane_height_);
                if (!plane)
                {
                    status_ = plane.error().message;
                }
            }
            if (navigation_pending_ && spatial_ && camera_ != lux::simulation::ecs::NullEntity &&
                camera_scene_ == editor_.instance())
            {
                const auto* pose = static_cast<const lux::simulation::ecs::Transform3D*>(
                    editor_.component(camera_, lux::cxx::typeToken<lux::simulation::ecs::Transform3D>())
                );
                const auto* camera = static_cast<const lux::scene::Camera*>(
                    editor_.component(camera_, lux::cxx::typeToken<lux::scene::Camera>())
                );
                if (!pose || !camera)
                    return;
                const auto candidate = spatial_->navigate(*pose, *camera, pending_motion_);
                const auto moved = candidate ? editor_.navigateCamera(camera_, candidate->transform, candidate->camera)
                                             : EditorResult<void>{lux::cxx::unexpected(candidate.error())};
                if (!moved && moved.error().code == EEditorError::BUSY)
                    return;
                navigation_pending_ = false;
                pending_motion_ = {};
                if (!moved)
                {
                    status_ = moved.error().message;
                }
            }
            const auto wanted_camera = editor_.viewportCamera();
            if (wanted_camera && *wanted_camera != camera_)
            {
                const auto bound = viewport_->setCamera(*wanted_camera);
                if (bound)
                    camera_ = *wanted_camera;
                else
                    remember("scene.view.camera", bound.error());
            }
            else if (!wanted_camera)
                status_ = wanted_camera.error().message;
            if (action_.index() != 0 && spatial_)
            {
                const Pick location =
                    action_.index() == 1 ? std::get<Pick>(action_) : std::get<Create>(action_).location;
                bool completed = true;
                if (location.instance != editor_.instance())
                {
                    status_ = "The action belongs to a previous Scene instance";
                }
                else if (!editor_.component(location.camera, lux::cxx::typeToken<lux::scene::Camera>()))
                {
                    status_ = "The action's camera no longer exists";
                }
                else if (action_.index() == 1 && editor_.selection().revision != location.selection_revision)
                {
                    status_ = "A newer selection superseded the pending pick";
                }
                else if (const auto* create = std::get_if<Create>(&action_))
                {
                    const auto history = editor_.historyView();
                    if (!history || history->history.current != create->base)
                    {
                        status_ = "Content changed after the model drop; repeat the operation";
                        action_.emplace<std::monostate>();
                        return;
                    }
                    lux::scene::RayHit3D hit;
                    const auto found = editor_.raycastNearest(location.instance, location.ray, 1.0e12, hit);
                    if (!found)
                    {
                        status_ = "Mesh query unavailable";
                        return;
                    }
                    const auto point = spatial_->creationPoint(*found ? &hit : nullptr, location.ray, create->plane);
                    if (!point)
                    {
                        status_ = point.error().message;
                        const auto* query = std::any_cast<lux::scene::MeshQueryFailure>(&point.error().cause);
                        completed = !query || query->code != lux::scene::EMeshQueryError::NOT_READY;
                    }
                    else
                    {
                        const auto accepted = editor_.requestModelCreation(create->asset, *point, create->partition);
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
                    const auto picked = editor_.raycastNearest(location.instance, location.ray, 1.0e12, hit);
                    if (!picked)
                    {
                        completed = picked.error().code != lux::scene::EMeshQueryError::NOT_READY;
                        status_ = "Mesh query " + std::to_string(static_cast<unsigned>(picked.error().code));
                    }
                    else
                    {
                        const auto selected = editor_.select(*picked ? hit.entity : lux::simulation::ecs::NullEntity);
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
    }

    void SceneContentElement::drawPlacement()
    {
        if (editor_.partitionCount() > 1)
        {
            const auto label = std::to_string(partition_);
            if (ImGui::BeginCombo("Partition", label.c_str()))
            {
                for (std::size_t index{}; index < editor_.partitionCount(); ++index)
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
        auto state = editor_.modelCreationStatus(placement_);
        if (!state)
        {
            status_ = state.error().domain + ": " + state.error().message;
            placement_ = {};
            return;
        }
        if (std::holds_alternative<scene::ModelCreationPending>(*state))
        {
            ImGui::TextDisabled("%s", "Loading model...");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel placement"))
            {
                detail_action_ = DetailAction{EDetailAction::CANCEL, {}, {}, placement_};
            }
        }
        else if (const auto* failure = std::get_if<EditorFailure>(&*state))
        {
            {
                const auto& message_value =
                    failure->domain + ": " + std::to_string(failure->reason) + " " + failure->message;
                const std::string_view message{message_value};
                ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.empty() ? "" : message.data());
            }
            if (ImGui::SmallButton("Retry placement"))
            {
                if (const auto history = editor_.historyView())
                    detail_action_ = DetailAction{EDetailAction::RETRY, {}, history->history.current, placement_};
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Discard placement"))
                detail_action_ = DetailAction{EDetailAction::DISCARD, {}, {}, placement_};
        }
    }

    void SceneContentElement::applyDetailAction() noexcept
    {
        if (closing_)
        {
            detail_action_.reset();
            return;
        }
        if (!detail_action_)
            return;
        const auto& action = *detail_action_;
        EditorResult<void> result;
        const bool camera_action =
            action.action == EDetailAction::PROJECTION || action.action == EDetailAction::CREATE_CAMERA;
        if (camera_action && action.scene_id != editor_.instance())
        {
            detail_action_.reset();
            return;
        }
        if (action.action == EDetailAction::PROJECTION)
        {
            const auto* pose = static_cast<const lux::simulation::ecs::Transform3D*>(
                editor_.component(action.camera, lux::cxx::typeToken<lux::simulation::ecs::Transform3D>())
            );
            const auto* camera = static_cast<const lux::scene::Camera*>(
                editor_.component(action.camera, lux::cxx::typeToken<lux::scene::Camera>())
            );
            if (!pose || !camera)
                result = lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "viewport.camera"});
            else
            {
                auto next = *camera;
                if (action.projection == 0)
                    next.projection = lux::scene::PerspectiveProjection{};
                else
                    next.projection = lux::scene::OrthographicProjection{};
                result = editor_.navigateCamera(action.camera, *pose, next);
            }
        }
        else if (action.action == EDetailAction::CREATE_CAMERA)
        {
            const auto created = editor_.createCameraFromView(action.camera, action.state, {action.partition});
            if (!created)
                result = lux::cxx::unexpected(EditorFailure{
                    created.error().code == editing::EEditError::BUSY ? EEditorError::BUSY
                                                                      : EEditorError::INVALID_STATE,
                    "viewport.camera.create",
                    static_cast<std::uint64_t>(created.error().code),
                    std::string(created.error().message.data()),
                    created.error()
                });
        }
        else if (action.action == EDetailAction::RETRY)
            result = editor_.retryModelCreation(action.placement, action.state);
        else
        {
            result = editor_.cancelModelCreation(action.placement);
            if (result && action.action == EDetailAction::DISCARD)
                result = editor_.acknowledgeModelCreation(action.placement);
        }
        if (!result)
            status_ = result.error().domain + ": " + result.error().message;
        if (result || result.error().code != EEditorError::BUSY)
            detail_action_.reset();
    }

    void SceneContentElement::applyControl() noexcept
    {
        if (render_selection_)
        {
            const auto selected = editor_.selectRenderSystem(*render_selection_);
            if (!selected)
                status_ = selected.error().domain;
            render_selection_.reset();
        }
        applyDetailAction();
        const auto control = std::exchange(control_, EControl::NONE);
        if (control == EControl::NONE || closing_)
            return;
        if (editor_.runStatus().id != control_run_)
            return;
        EditorResult<void> result;
        switch (control)
        {
        case EControl::PLAY:
            if (auto started = editor_.play(); !started)
                result = lux::cxx::unexpected(started.error());
            break;
        case EControl::PAUSE:
            result = editor_.pauseRun(control_run_);
            break;
        case EControl::RESUME:
            result = editor_.resumeRun(control_run_);
            break;
        case EControl::STEP:
            result = editor_.stepRun(control_run_);
            break;
        case EControl::STOP:
            result = editor_.stopRun(control_run_);
            break;
        case EControl::NONE:
            break;
        }
        if (!result)
            status_ = result.error().domain + ": " + result.error().message;
    }

    SceneContentElement::Details::Details(SceneContentElement& owner)
        : lux::ui::Element(owner.layout_, lux::ui::ElementId{"details"}), owner_(owner)
    {
        setStretch({1, 0});
    }
    lux::ui::SizeHint SceneContentElement::Details::sizeHintContent() noexcept
    {
        const auto height = ImGui::GetFrameHeightWithSpacing() * 3;
        return {{0, height}, {480, height}, {std::numeric_limits<float>::infinity(), height}};
    }
    lux::ui::SizeHint SceneContentElement::sizeHintContent() noexcept
    {
        return layout_.sizeHint();
    }
    lux::ui::SizeHint SceneContentElement::measureContent(float width) noexcept
    {
        return layout_.measure(width);
    }
    void SceneContentElement::arrangeContent() noexcept
    {
        layout_.arrange({{}, rect().size});
    }

    void SceneContentElement::drawDetails() noexcept
    {
        if (source_)
        {
            const auto& description = source_->scene->data();
            const auto borrowed = std::as_const(runtime_).getSceneRegistry(editor_.instance());
            const auto selected = description.findSystem(editor_.selectedRenderSystem());
            if (ImGui::BeginCombo("Viewport system", selected ? selected.instanceName().data() : "No render system"))
            {
                for (std::size_t i{}; i < description.systemCount(); ++i)
                {
                    const auto system = description.systemAt(i);
                    if (!borrowed || !lux::scene::RenderAssets::find(borrowed->get(), system.instanceId()))
                        continue;
                    if (ImGui::Selectable(
                            system.instanceName().data(),
                            system.instanceId() == editor_.selectedRenderSystem()
                        ))
                        render_selection_ = system.instanceId();
                }
                ImGui::EndCombo();
            }
        }
        const auto run = editor_.runStatus();
        if (!displayed_run_.serial && camera_ != lux::simulation::ecs::NullEntity &&
            camera_scene_ == editor_.instance())
        {
            const auto* camera = static_cast<const lux::scene::Camera*>(
                editor_.component(camera_, lux::cxx::typeToken<lux::scene::Camera>())
            );
            const auto* pose = static_cast<const lux::simulation::ecs::Transform3D*>(
                editor_.component(camera_, lux::cxx::typeToken<lux::simulation::ecs::Transform3D>())
            );
            if (camera && pose)
            {
                ImGui::SameLine();
                int projection = static_cast<int>(camera->projection.index());
                ImGui::SetNextItemWidth(140.0F);
                if (ImGui::Combo("##projection", &projection, "Perspective\0Orthographic\0"))
                {
                    detail_action_ =
                        DetailAction{EDetailAction::PROJECTION, camera_, {}, {}, 0, projection, camera_scene_};
                }
                ImGui::SameLine();
                if (ImGui::Button("Create game camera"))
                {
                    if (const auto history = editor_.historyView())
                        detail_action_ = DetailAction{
                            EDetailAction::CREATE_CAMERA,
                            camera_,
                            history->history.current,
                            {},
                            partition_,
                            0,
                            camera_scene_
                        };
                }
            }
        }
        if (!run.result)
        {
            {
                const auto& message_value = run.result.error().domain + ": " + run.result.error().message;
                const std::string_view message{message_value};
                ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.empty() ? "" : message.data());
            }
        }
        if (closing_ || !view().isValid())
        {
            {
                const auto& message_value = status_.empty() ? "Preparing scene view..." : status_;
                const std::string_view message{message_value};
                ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.empty() ? "" : message.data());
            }
            if (!closing_ && !view_result_ && ImGui::SmallButton("Reopen view"))
            {
                reopen_ = true;
            }
            return;
        }
        const auto view_status = viewport_->observation().status;
        if (view_status.failure)
        {
            remember("scene.view.status", *view_status.failure);
        }
        const auto& next = viewport_->result();
        if (!next && next.error().code != lux::render::ERendererError::NOT_READY)
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
            drawPlacement();
        }
    }

    void SceneContentElement::draw() noexcept
    {
        drawChild(layout_);
        if (!viewport_ || closing_ || !view().isValid())
            return;
        const auto& interaction = viewport_->image().interaction();
        if (!displayed_run_.serial && interaction.hovered && ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload(kAssetReferencePayload))
            {
                const auto reference = decodeAssetReference(
                    {static_cast<const std::byte*>(payload->Data), static_cast<std::size_t>(payload->DataSize)}
                );
                if (!reference)
                {
                    status_ = reference.error().domain + ": " + reference.error().message;
                }
                else if (spatial_)
                {
                    const auto ray = cameraRay(
                        {interaction.local_pointer.x, interaction.local_pointer.y},
                        {interaction.size.width, interaction.size.height}
                    );
                    if (!ray)
                    {
                        status_ = ray.error().message;
                    }
                    else
                    {
                        action_.emplace<Create>(
                            Pick{editor_.instance(), *ray, camera_, editor_.selection().revision},
                            *reference,
                            lux::partition::PartitionOrdinal{partition_},
                            work_plane_height_,
                            editor_.historyView()->history.current
                        );
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        const auto& input = ImGui::GetIO();
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
            const auto ray = cameraRay(
                {interaction.local_pointer.x, interaction.local_pointer.y},
                {interaction.size.width, interaction.size.height}
            );
            if (ray)
            {
                action_.emplace<Pick>(editor_.instance(), *ray, camera_, editor_.selection().revision);
            }
            else
            {
                status_ = ray.error().message;
            }
        }
    }

    void SceneContentElement::requestClose() noexcept
    {
        control_ = EControl::NONE;
        render_selection_.reset();
        detail_action_.reset();
        closing_ = true;
        setVisible(false);
    }

    void SceneContentElement::reopen() noexcept
    {
        if (!viewport_)
        {
            closing_ = closed_ = false;
            camera_ = lux::simulation::ecs::NullEntity;
            camera_scene_ = {};
            displayed_run_ = {};
            view_result_ = {};
            status_.clear();
            placement_ = {};
            detail_action_.reset();
            action_.emplace<std::monostate>();
            control_ = EControl::NONE;
            setVisible(true);
        }
    }

    CloseStatus SceneContentElement::closeStatus() const
    {
        return {closed_ ? ECloseState::CLOSED : closing_ ? ECloseState::CLOSING : ECloseState::OPEN, status_};
    }
} // namespace lux::editor::ui
