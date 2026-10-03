#include <lux/engine/editor/application/Launcher.hpp>
#include <lux/engine/editor/application/ProjectCreation.hpp>
#include <lux/engine/editor/desktop/DesktopShell.hpp>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/FileDialog.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <cstdio>

namespace
{
    constexpr lux::editor::commands::CommandDescriptor command_lux_project_open{
        lux::editor::commands::CommandIdView{"lux.project.open"},
        "Open Project",
        "File"
    };
}
namespace lux::editor::application
{
    int runLauncher(const std::filesystem::path& installation, unsigned smoke_frames)
    {
        // Reverse destruction keeps the native surface, renderer and execution alive through all UI/task owners.
        window::GlfwRuntime platform;
        if (!platform.valid())
            return 3;
        auto displays = window::LuxWindow::displays();
        if (!displays)
            return 3;
        auto placement = window::resolveWindowPlacement({}, *displays);
        if (!placement)
            return 3;
        const auto normal = placement->placement.normal;
        window::LuxWindow window(normal.width, normal.height, "Lux Launcher");
        if (!window.isInitialized() || !window.applyPlacement(placement->placement))
            return 3;
        auto engine =
            engine::EngineContext::create({2, 128, 128, {128}, process::BlockingSchedulerConfig{2, 64}}, {0, 1024});
        if (!engine)
            return 3;
        if (!engine::initializeRendering(**engine, window::LuxWindow::requiredVulkanInstanceExtensions()))
            return 3;
        auto& rendering = *(*engine)->renderContext();
        if (!rendering.registerFeatures({render::kUiRenderRenderFeatureRegistration}))
            return 3;
        auto messages = object::ObjectMessageQueue::create(256);
        if (!messages)
            return 3;
        auto& execution = (*engine)->execution();
        ProjectCreation creation(execution, messages->dispatcherRef(), installation);
        commands::CommandRegistry commands;
        commands::CommandDispatcher dispatcher(commands);
        bool closing{}, open_requested{};
        std::optional<EditorResult<void>> launched;
        std::optional<process::TaskId> launching;
        process::TaskScope tasks(execution);
        auto desktop = desktop::DesktopShell::create(
            messages->dispatcherRef(),
            execution,
            (*engine)->sceneRuntime(),
            rendering.runtime(),
            rendering.resources(),
            &window
        );
        if (!desktop)
            return 3;
        EditorResult<void> constructed;
        auto view = std::make_unique<project::ProjectCreationView>(
            messages->dispatcherRef(),
            lux::ui::PaneId{"project-creation"},
            creation.requests(),
            constructed
        );
        if (!constructed)
            return 3;
        view->setModal(false);
        views::DetachedView candidate{contracts::CodeLease::builtin(), std::move(view)};
        auto adopted = (*desktop)->views().adopt(candidate, views::ViewRestoreKey{"project-creation"});
        if (!adopted)
            return 3;
        std::vector<std::shared_ptr<commands::CommandEntry>> entries;
        entries.push_back(commands::CommandEntry::bind<command_lux_project_open>(
            contracts::CodeLease::builtin(),
            [&](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{!closing && !launching};
            },
            [&](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                open_requested = true;
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        auto catalog = commands::CommandRegistrySnapshot::create(std::move(entries));
        if (!catalog || !commands.publish(std::move(*catalog)))
            return 3;
        if (!(*desktop)->installCommands(
                commands,
                dispatcher,
                [](const auto&, auto*, auto*) -> commands::CommandResult<commands::CommandInvocation> {
                    return commands::CommandInvocation{};
                }
            ))
            return 3;
        if (!creation.start())
            return 3;
        window.on_close = [&](const window::WindowCloseEvent&) { closing = true; };
        input::Input input;
        int outcome{};
        const auto started = std::chrono::steady_clock::now();
        auto fail = [&](std::string_view operation) {
            std::fprintf(stderr, "launcher.%.*s failed\n", static_cast<int>(operation.size()), operation.data());
            outcome = 3;
            closing = true;
        };
        for (;;)
        {
            window::LuxWindow::pollEvents();
            if (!execution.collectCompletions())
                fail("completions");
            if (!execution.dispatchTaskEvents())
                fail("tasks");
            (void)messages->dispatchPending();
            creation.update();
            if (launched)
            {
                if (!*launched)
                    fail(launched->error().domain);
                else
                    closing = true;
                launched.reset();
            }
            if (creation.progress().launched)
                closing = true;
            auto intents = (*desktop)->views().closeIntents();
            if (!intents)
                fail("close");
            else if (!intents->empty())
                closing = true;
            if (closing)
            {
                window.hide(true);
                creation.cancel();
                if (!creation.progress().pending && !launching)
                    break;
            }
            else
            {
                input.sample(window);
                if (!(*desktop)->feedInput(input.snapshot()))
                    fail("input");
            }
            if (!(*desktop)->update(closing ? std::optional{lux::ui::FrameInfo{}} : std::nullopt))
                fail("desktop");
            for (const auto& completion : (*desktop)->commands()->takeCompletions())
                if (!completion.result)
                    fail(completion.result.error().domain);
            if (!closing && std::exchange(open_requested, false))
            {
                const std::array filters{window::FileDialogFilter{"Lux project", "luxproject"}};
                auto chosen = window::openFileDialog(&window, filters);
                if (!chosen)
                    fail("dialog");
                else if (*chosen)
                {
                    auto accepted = tasks.submit(
                        {"Open project", "Project"},
                        [installation,
                         file = std::move(**chosen),
                         scheduler = *execution.blocking()](process::TaskReporter) noexcept {
                            return stdexec::then(stdexec::schedule(scheduler), [installation, file]() noexcept {
                                return launchEditor(installation, file);
                            });
                        },
                        [&](process::TTaskResult<void, EditorFailure>&& result) noexcept {
                            launching.reset();
                            if (result)
                                launched.emplace();
                            else if (auto* error = result.error().domainFailure())
                                launched.emplace(cxx::unexpected(std::move(*error)));
                            else
                                launched.emplace(
                                    cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE, "launcher.task"})
                                );
                        }
                    );
                    if (!accepted)
                        fail("submit");
                    else
                        launching = *accepted;
                }
            }
            auto driven = (*engine)->sceneRuntime().driveFrame();
            if (!driven || !driven->empty())
                fail("scenes");
            if (smoke_frames && (*desktop)->presentation().capturedFrames() >= smoke_frames)
                closing = true;
            if (smoke_frames && std::chrono::steady_clock::now() - started > std::chrono::seconds(30))
                fail("smoke.timeout");
            window::LuxWindow::waitEvents(0.001);
        }
        return outcome;
    }
}
