#include <algorithm>
#include <lux/engine/editor/gui/scene/RunPane.hpp>

namespace lux::editor::gui
{
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
            setVisible(true);
        }
        if (auto *preview = std::get_if<Preview>(&preview_))
        {
            if (closing_ || !visible() || !active || preview->run != run.id)
            {
                preview->image = {};
                auto &view = preview->lease.view();
                const auto requested = view.beginClose();
                if (!requested)
                {
                    status_ = "Run view close was not accepted";
                    return;
                }
                const auto closed = view.advanceClose();
                if (!closed)
                {
                    status_ = "Run view close failed; owner retained";
                    return;
                }
                if (*closed == rendering::ERenderClose::COMPLETE)
                {
                    preview_.emplace<Idle>();
                }
            }
            return;
        }
        if (!closing_ && visible() && active)
        {
            auto opened = document_.openRunView(run.id, {{640, 480}, true, document_.runCoordinatePageSize()});
            if (!opened)
            {
                status_ = opened.error().domain + ": " + opened.error().message;
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
            return {ECloseState::CLOSING, status_};
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
        if (ImGui::Button("Stop"))
        {
            action(document_.stopRun(run.id));
        }
        ImGui::SameLine();
        ImGui::Text("Step %llu | %.3f s", static_cast<unsigned long long>(run.steps),
                    std::chrono::duration<double>(run.elapsed).count());
        if (!status_.empty())
        {
            frame.textWrapped(status_);
        }
        auto *preview = std::get_if<Preview>(&preview_);
        if (!preview)
        {
            frame.textMuted("Preparing or closing Run view...");
            return;
        }
        auto &view = preview->lease.view();
        auto image = view.acquireImage();
        if (image)
        {
            preview->image = std::move(*image);
        }
        const auto interaction = viewport_.draw(frame, {preview->image.texture});
        const auto &io = ImGui::GetIO();
        const rendering::PixelExtent extent{
            static_cast<std::uint32_t>((std::max)(0.F, interaction.size.width * io.DisplayFramebufferScale.x)),
            static_cast<std::uint32_t>((std::max)(0.F, interaction.size.height * io.DisplayFramebufferScale.y))};
        const auto resized = view.requestExtent(extent);
        if (!resized)
        {
            status_ = "Run view resize failed";
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
            if (view.setCamera(camera))
            {
                preview->camera_extent = extent;
            }
        }
    }

    void RunPane::appendFrameImages(std::vector<rendering::ViewImage> &images) const
    {
        const auto *preview = std::get_if<Preview>(&preview_);
        if (preview && visible() && preview->image.lease.valid())
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
