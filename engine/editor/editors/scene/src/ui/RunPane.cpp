#include <algorithm>
#include <lux/engine/editor/gui/scene/RunPane.hpp>

namespace lux::editor::gui
{
    void RunPane::remember(std::string_view operation, const rendering::RendererFailure &failure)
    {
        if (!view_result_)
        {
            const auto *previous = std::any_cast<rendering::RendererFailure>(&view_result_.error().cause);
            if (previous && previous->code == failure.code && previous->view == failure.view &&
                previous->request == failure.request && previous->render_error.type == failure.render_error.type &&
                previous->render_error.args == failure.render_error.args &&
                previous->backend_status == failure.backend_status)
            {
                return;
            }
        }
        view_result_ = lux::cxx::unexpected(EditorFailure{
            EEditorError::FRONTEND_FAILURE, std::string(operation), static_cast<std::uint64_t>(failure.code),
            "Renderer " + std::to_string(static_cast<unsigned>(failure.code)) + ", view " +
                std::to_string(failure.view.slot) + ":" + std::to_string(failure.view.generation) + ", request " +
                std::to_string(failure.request) + ", backend " + std::to_string(failure.render_error.type.index) + ":" +
                std::to_string(failure.render_error.type.gen),
            failure});
    }

    RunPane::RunPane(scene::SceneEditor &document, std::string id) : DocumentPane(document, std::move(id), "Run")
    {
        setVisible(false);
    }

    void RunPane::poll(PollBudget &budget)
    {
        DocumentPane::poll(budget);
        const auto run = document_.runStatus();
        const bool active = run.state == scene::ERunState::RUNNING || run.state == scene::ERunState::PAUSED;
        if (active && observed_ != run.id && !closing_)
        {
            observed_ = run.id;
            view_result_ = {};
            setVisible(true);
        }
        if (auto *preview = std::get_if<Preview>(&preview_))
        {
            const auto state = preview->lease.view().status();
            if (state.failure)
            {
                remember("run.view", *state.failure);
            }
            if (view_closing_ || closing_ || !visible() || !active || preview->run != run.id)
            {
                view_closing_ = true;
                preview->image = {};
                auto &view = preview->lease.view();
                const auto requested = view.beginClose();
                if (!requested)
                {
                    remember("run.view.beginClose", requested.error());
                    return;
                }
                const auto closed = view.advanceClose();
                if (!closed)
                {
                    remember("run.view.advanceClose", closed.error());
                    return;
                }
                if (*closed == rendering::ERenderClose::COMPLETE)
                {
                    preview_.emplace<Idle>();
                    view_closing_ = false;
                    if (std::exchange(reopen_requested_, false))
                    {
                        view_result_ = {};
                    }
                }
            }
            return;
        }
        if (!closing_ && visible() && active && view_result_)
        {
            auto opened = document_.openRunView(run.id, {{640, 480}, true, document_.runCoordinatePageSize()});
            if (!opened)
            {
                view_result_ = lux::cxx::unexpected(opened.error());
                return;
            }
            preview_.emplace<Preview>(run.id, std::move(*opened), rendering::ViewImage{}, rendering::PixelExtent{});
            status_.clear();
        }
    }

    CloseStatus RunPane::closeStatus() const
    {
        if (!closing_)
        {
            return {ECloseState::OPEN, status_};
        }
        if (preview_.index() != 0)
        {
            return {ECloseState::CLOSING, view_result_ ? "Run view: waiting for CPU references and GPU retirement"
                                                       : view_result_.error().message};
        }
        return DocumentPane::closeStatus();
    }

    void RunPane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        ImGui::SetWindowSize({720, 520}, ImGuiCond_FirstUseEver);
        context.activateContext(lux::ui::UiContextIdView{id()});
        const auto run = document_.runStatus();
        const auto action = [&](EditorResult<void> result)
        {
            if (!result)
            {
                status_ = result.error().domain + ": " + result.error().message;
            }
        };
        if (run.pause_pending)
        {
            frame.textMuted("Pause pending: finishing the current stable point...");
        }
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
        ImGui::BeginDisabled(run.state == scene::ERunState::IDLE || run.state == scene::ERunState::FINISHED ||
                             run.state == scene::ERunState::FAILED || run.state == scene::ERunState::STOPPING);
        if (ImGui::Button("Stop"))
        {
            action(document_.stopRun(run.id));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("Step %llu | %.3f s", static_cast<unsigned long long>(run.steps),
                    std::chrono::duration<double>(run.elapsed).count());
        if (!status_.empty())
        {
            frame.textWrapped(status_);
        }
        if (!view_result_)
        {
            frame.textWrapped(view_result_.error().domain + ": " + view_result_.error().message);
            if (!closing_ && ImGui::Button("Reopen view"))
            {
                // Only the Pane view is replaced. The independent Run keeps its
                // identity/history.
                view_closing_ = preview_.index() != 0;
                reopen_requested_ = view_closing_;
                view_result_ = {};
            }
        }
        if (view_closing_)
        {
            frame.textMuted("Closing Run view: waiting for CPU references and "
                            "GPU retirement...");
            return;
        }
        auto *preview = std::get_if<Preview>(&preview_);
        if (!preview)
        {
            if (view_result_)
            {
                switch (run.state)
                {
                case scene::ERunState::IDLE:
                    frame.textMuted("No active Run. Use Play in the Scene pane.");
                    break;
                case scene::ERunState::FINISHED:
                    frame.textMuted("Run finished.");
                    break;
                case scene::ERunState::FAILED:
                    frame.textWrapped(run.result.error().domain + ": " + run.result.error().message);
                    break;
                case scene::ERunState::STOPPING:
                    frame.textMuted("Stopping Run...");
                    break;
                default:
                    frame.textMuted("Preparing Run view...");
                    break;
                }
            }
            return;
        }
        auto &view = preview->lease.view();
        auto image = view.acquireImage();
        if (image)
        {
            preview->image = std::move(*image);
        }
        else if (image.error().code == rendering::ERendererError::NOT_READY)
        {
            if (view_result_)
            {
                frame.textMuted("Run image not ready...");
            }
        }
        else
        {
            remember("run.view.acquireImage", image.error());
        }
        if (view.status().state == rendering::EViewState::FAILED)
        {
            return;
        }
        const auto interaction = viewport_.draw(frame, {preview->image.texture});
        const auto &io = ImGui::GetIO();
        const rendering::PixelExtent extent{
            static_cast<std::uint32_t>((std::max)(0.F, interaction.size.width * io.DisplayFramebufferScale.x)),
            static_cast<std::uint32_t>((std::max)(0.F, interaction.size.height * io.DisplayFramebufferScale.y))};
        const auto resized = view.requestExtent(extent);
        if (!resized)
        {
            remember("run.view.requestExtent", resized.error());
        }
        if (extent.width && extent.height && extent != preview->camera_extent)
        {
            const auto origin = camera_.position();
            const auto matrix = camera_.view(origin);
            const auto projection = camera_.projection(double(extent.width) / extent.height);
            if (!projection)
            {
                status_ = "Run camera projection failed";
                return;
            }
            rendering::CameraFrame camera;
            std::copy_n(matrix.data(), 16, camera.view.begin());
            std::copy_n(projection->data(), 16, camera.projection.begin());
            std::copy_n(origin.data(), 3, camera.origin.begin());
            camera.desired = {run.id.serial, 0, 1, 1};
            const auto updated = view.setCamera(camera);
            if (updated)
            {
                preview->camera_extent = extent;
            }
            else
            {
                remember("run.view.setCamera", updated.error());
            }
        }
    }

    void RunPane::appendFrameImages(std::vector<rendering::ViewImage> &images) const
    {
        const auto *preview = std::get_if<Preview>(&preview_);
        // A close click may hide the Pane after its image was drawn this frame.
        // Retain the draw's references until the frontend seals that snapshot.
        if (preview && preview->image.lease.valid())
        {
            images.push_back(preview->image);
        }
    }
    void RunPane::releaseFrameImages() noexcept
    {
        if (auto *preview = std::get_if<Preview>(&preview_))
        {
            preview->image = {};
        }
    }
} // namespace lux::editor::gui
