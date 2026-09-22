#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <thread>

namespace lux::editor
{
namespace
{
struct ScopeClosed final
{
    using receiver_concept = stdexec::receiver_t;
    std::atomic<bool> *done;
    stdexec::empty_env get_env() const noexcept
    {
        return {};
    }
    void set_value() && noexcept
    {
        done->store(true, std::memory_order_release);
    }
};
} // namespace

void Editor::pumpRender(PollBudget &budget)
{
    if (!impl_ || !impl_->renderer)
    {
        return;
    }
    auto &renderer = *impl_->renderer;
    auto result = renderer.poll(budget.render_replies, budget.render_controls, budget.render_programs);
    if (!result)
    {
        fail({EEditorError::FRONTEND_FAILURE, "editor.render.poll", 0, {}, result.error()});
    }
    else
    {
        budget.render_replies -= *result;
    }
    const auto state = renderer.status();
    if (!state.error.ok())
    {
        fail({EEditorError::FRONTEND_FAILURE, "editor.render.terminal", 0, {}, state.error});
    }
}

void Editor::advanceUi(PollBudget &budget)
{
    if (!impl_ || !impl_->ui_scene)
    {
        return;
    }
    scene::SceneAdvanceBudget advance{budget.system_calls, budget.document_steps, budget.render_programs,
                                      budget.resource_steps};
    static_cast<void>(impl_->driver.advance(*impl_->ui_scene, std::chrono::steady_clock::now(), advance));
    budget.system_calls = advance.system_calls;
    budget.document_steps = advance.new_steps;
    budget.render_programs = advance.publications;
    budget.resource_steps = advance.resource_steps;
    const auto &progress = impl_->ui_scene->progress();
    if (!progress.result)
    {
        fail({EEditorError::FRONTEND_FAILURE, "editor.ui.advance", 0, {}, progress.result.error()});
    }
}

void Editor::drawUi()
{
    if (!impl_ || !impl_->ui || exit_requested_ || !impl_->ui->canBuildFrame())
    {
        return;
    }
    auto &d = *impl_;
    if (d.ui->resourceStatus().state != render::ESceneResourceState::READY)
    {
        return;
    }
    int width{}, height{}, pixels_x{}, pixels_y{};
    glfwGetWindowSize(d.window->handle(), &width, &height);
    glfwGetFramebufferSize(d.window->handle(), &pixels_x, &pixels_y);
    if (!d.output)
    {
        render::ViewConfig view{.extent = {std::uint32_t(std::max(pixels_x, 0)), std::uint32_t(std::max(pixels_y, 0))}};
#if defined(_WIN32)
        view.output = render::NativeSurfaceOutput{reinterpret_cast<std::uintptr_t>(d.window->win32Handle())};
#else
        fail({EEditorError::FRONTEND_FAILURE, "editor.surface", 0,
              "Native surface is not implemented on this platform"});
        return;
#endif
        auto opened = d.ui->openView(view);
        if (!opened)
        {
            fail({EEditorError::FRONTEND_FAILURE, "editor.surface.open", 0, {}, opened.error()});
            return;
        }
        d.output = std::move(*opened);
    }
    auto resized =
        d.output->requestExtent({std::uint32_t(std::max(pixels_x, 0)), std::uint32_t(std::max(pixels_y, 0))});
    if (!resized)
    {
        fail({EEditorError::FRONTEND_FAILURE, "editor.surface.resize", 0, {}, resized.error()});
        return;
    }
    if (width <= 0 || height <= 0 || pixels_x <= 0 || pixels_y <= 0 ||
        d.output->status().state != render::EViewState::READY)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<float>(now - d.last_frame).count();
    d.last_frame = now;
    const lux::ui::Size extent{float(width), float(height)};
    auto frame = d.ui->beginFrame(
        {extent, std::clamp(elapsed, 0.001F, 0.1F), {float(pixels_x) / width, float(pixels_y) / height}});
    d.ui->drawPanes(frame);
    const auto input = d.ui->inputSnapshot();
    const bool ctrl =
        input.held[std::size_t(lux::ui::EKey::LEFT_CONTROL)] || input.held[std::size_t(lux::ui::EKey::RIGHT_CONTROL)];
    if (input.window_focused && !input.keyboard_blocked && !input.modal_open && ctrl)
    {
        auto &router = d.ui->commandRouter();
        const auto invoke = [&](lux::ui::EKey key, lux::ui::UiCommandIdView id) {
            if (input.pressed[std::size_t(key)])
            {
                if (const auto command = router.findCommand(id))
                {
                    static_cast<void>(router.invoke(*command));
                }
            }
        };
        invoke(lux::ui::EKey::Z, lux::ui::UiCommandIdView{"lux.edit.undo"});
        invoke(lux::ui::EKey::Y, lux::ui::UiCommandIdView{"lux.edit.redo"});
    }
    d.images.clear();
    d.visitViews(*this, [&](gui::GuiView &view) { view.appendFrameImages(d.images); });
    auto finished = d.ui->finishFrame(frame, d.images);
    if (!finished)
    {
        fail({EEditorError::FRONTEND_FAILURE, "editor.ui.capture", 0, {}, finished.error()});
        return;
    }
    d.images.clear();
    d.visitViews(*this, [](gui::GuiView &view) { view.releaseFrameImages(); });
    positionTextInput(extent);
    d.driver.invalidate(*d.ui_scene);
}

void Editor::stopDesktop() noexcept
{
    if (!impl_ || impl_->frames_stopped)
    {
        return;
    }
    auto &d = *impl_;
    d.frames_stopped = true;
    d.images.clear();
    d.visitViews(*this, [](gui::GuiView &view) { view.releaseFrameImages(); });
    if (d.ui)
    {
        d.ui->stopFrames();
        d.driver.invalidate(*d.ui_scene);
    }
}

bool Editor::closeDesktop(PollBudget &budget)
{
    if (!impl_)
    {
        return true;
    }
    auto &d = *impl_;
    if (!d.desktop_stopping)
    {
        stopDesktop();
        clearPlatformInput();
        d.asset_open.reset();
        d.project_registration.reset();
        d.project_pane.reset();
        d.output.reset();
        d.ui = nullptr;
        d.ui_scene.reset();
        d.desktop_stopping = true;
        if (d.renderer)
        {
            const auto closing = d.renderer->beginClose();
            if (!closing)
            {
                fail({EEditorError::FRONTEND_FAILURE, "editor.render.close", 0, {}, closing.error()});
            }
        }
    }
    if (d.renderer && !d.renderer_joined)
    {
        const auto closed =
            d.renderer->advanceClose(budget.render_replies, budget.render_controls, budget.render_programs);
        if (!closed)
        {
            fail({EEditorError::FRONTEND_FAILURE, "editor.render.retire", 0, {}, closed.error()});
            return false;
        }
        if (*closed != render::ERenderClose::COMPLETE)
        {
            return false;
        }
        const auto joined = d.renderer->joinStopped();
        if (!joined)
        {
            fail({EEditorError::FRONTEND_FAILURE, "editor.render.join", 0, {}, joined.error()});
            return false;
        }
        d.renderer_joined = true;
    }
    d.window.reset(); // The native surface and every accepted GPU use have retired.
    return true;
}

void Editor::wait()
{
    if (impl_ && impl_->platform.valid())
    {
        glfwWaitEventsTimeout(0.002);
    }
    else
    {
        std::this_thread::yield();
    }
}

int Editor::exec()
{
    if (state_ != EState::COLD || config_.project_file.empty() || !config_.limits.documents ||
        !config_.limits.open_requests || !config_.limits.turn.document_steps || !config_.limits.turn.system_calls ||
        !config_.limits.turn.main_completions || !config_.limits.turn.render_replies ||
        !config_.limits.turn.object_messages || !config_.limits.turn.render_programs ||
        !config_.limits.turn.render_controls || !config_.limits.turn.resource_steps)
    {
        return 2;
    }
    state_ = EState::RUNNING;
    auto process = process::ExecutionRuntime::create(config_.execution);
    if (!process)
    {
        state_ = EState::FINISHED;
        return 3;
    }
    const auto blocking = process->blocking();
    if (!blocking)
    {
        process->requestStop();
        static_cast<void>(process->join());
        state_ = EState::FINISHED;
        return 3;
    }
    process::TaskScope tasks;
    EditorResult<ProjectSource> source = lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "startup"});
    lux::ui::UiFontSource font;
    std::atomic<bool> ready{};
    auto work = stdexec::then(stdexec::schedule(*blocking), [&]() noexcept {
        if (config_.window.font)
        {
            const auto &spec = *config_.window.font;
            std::ifstream input(spec.file, std::ios::binary | std::ios::ate);
            const auto size = input ? std::streamoff(input.tellg()) : -1;
            if (size <= 0 || size > 32 * 1024 * 1024)
            {
                source = lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "editor.font.read"});
                ready.store(true, std::memory_order_release);
                return;
            }
            font.bytes.resize(std::size_t(size));
            font.ranges = spec.ranges;
            font.face = spec.face;
            font.size_pixels = spec.size_pixels;
            input.seekg(0);
            if (!input.read(reinterpret_cast<char *>(font.bytes.data()), size))
            {
                source = lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "editor.font.read"});
                ready.store(true, std::memory_order_release);
                return;
            }
        }
        source = readProjectSource(config_.project_file);
        ready.store(true, std::memory_order_release);
    });
    auto errors = stdexec::upon_error(std::move(work), [&](process::EExecutionError error) noexcept {
        source = lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE, "startup.schedule", 0, {}, error});
        ready.store(true, std::memory_order_release);
    });
    auto stopped = stdexec::upon_stopped(std::move(errors), [&]() noexcept {
        source = lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "startup"});
        ready.store(true, std::memory_order_release);
    });
    auto admitted = tasks.start(std::move(stopped));
    if (!admitted)
    {
        source = lux::cxx::unexpected(
            EditorFailure{EEditorError::EXECUTION_FAILURE, "startup.admission", 0, {}, admitted.error()});
        ready.store(true, std::memory_order_release);
    }
    while (!ready.load(std::memory_order_acquire))
    {
        static_cast<void>(process->drainMain(config_.limits.turn.main_completions));
        std::this_thread::yield();
    }

    std::unique_ptr<Project> project;
    if (!source)
    {
        fail(source.error());
    }
    else
    {
        auto opened = Project::open(*source, *blocking, tasks, messages_.dispatcherRef());
        if (!opened)
        {
            fail(opened.error());
        }
        else
        {
            project = std::move(*opened);
            project_ = project.get();
            auto started = startDesktop(*process, config_.window.font ? &font : nullptr);
            if (!started)
            {
                fail(started.error());
            }
        }
    }

    // Each turn has one UI build and one owner adoption boundary. A full render
    // queue prevents another build, not input, Process completion, or Stop.
    while (!exit_requested_ || !documents_.empty() || !openings_.empty())
    {
        auto budget = config_.limits.turn;
        collectInput();
        const auto main = process->drainMain(budget.main_completions);
        if (main)
        {
            budget.main_completions -= *main;
        }
        budget.object_messages -= messages_.dispatchPending(budget.object_messages);
        acceptOpenings(budget);
        collectClosed();
        if (impl_ && impl_->project_pane)
        {
            impl_->project_pane->poll();
        }
        if (!exit_requested_)
        {
            drawUi();
        }
        if (exit_requested_)
        {
            state_ = EState::CLOSING;
            stopDesktop();
            for (const auto &opening : openings_)
            {
                opening->waiters.clear();
                opening->work->cancel();
            }
            for (const auto &document : documents_.values())
            {
                document->requestClose();
            }
        }
        advanceOwners(budget);
        collectClosed();
        wait();
    }
    stopDesktop();
    registrations_.clear();
    while (true)
    {
        auto budget = config_.limits.turn;
        static_cast<void>(process->drainMain(budget.main_completions));
        if (closeDesktop(budget))
        {
            break;
        }
        wait();
    }
    impl_.reset();
    if (project)
    {
        project->requestClose();
        while (true)
        {
            const auto closed = project->advanceClose();
            if (closed && *closed)
            {
                break;
            }
            if (!closed)
            {
                fail(closed.error());
            }
            static_cast<void>(process->drainMain(config_.limits.turn.main_completions));
            std::this_thread::yield();
        }
        project_ = nullptr;
        project.reset();
    }
    std::atomic<bool> complete{};
    auto close = stdexec::connect(tasks.close(), ScopeClosed{&complete});
    stdexec::start(close);
    while (!complete.load(std::memory_order_acquire))
    {
        static_cast<void>(process->drainMain(config_.limits.turn.main_completions));
        std::this_thread::yield();
    }
    messages_.close();
    process->requestStop();
    while (true)
    {
        const auto drained = process->drainMain(config_.limits.turn.main_completions);
        if (!drained || !*drained)
        {
            break;
        }
    }
    const auto joined = process->join();
    state_ = EState::FINISHED;
    const int result = joined ? (outcome_ ? 0 : 5) : 7;
    std::fprintf(stderr, "[editor.exit] event=finished code=%d failure=%u joined=%u\n", result, unsigned(!outcome_),
                 unsigned(joined.has_value()));
    return result;
}
} // namespace lux::editor
