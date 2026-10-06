#include <cstdio>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/editor/application/Launcher.hpp>
#include <lux/engine/editor/desktop/DesktopShell.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/editor/project/ProjectCreationView.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/window/FileDialog.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

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
        {
            return 3;
        }
        auto displays = window::LuxWindow::displays();
        if (!displays)
        {
            return 3;
        }
        auto placement = window::resolveWindowPlacement({}, *displays);
        if (!placement)
        {
            return 3;
        }
        const auto normal = placement->placement.normal;
        window::LuxWindow window(normal.width, normal.height, "Lux Launcher");
        if (!window.isInitialized() || !window.applyPlacement(placement->placement))
        {
            return 3;
        }
        auto engine =
            engine::EngineContext::create({2, 128, 128, {128}, process::BlockingSchedulerConfig{2, 64}}, {0, 1024});
        if (!engine)
        {
            return 3;
        }
        if (!engine::initializeRendering(**engine, window::LuxWindow::requiredVulkanInstanceExtensions()))
        {
            return 3;
        }
        auto& rendering = *(*engine)->renderContext();
        if (!rendering.registerFeatures({render::kUiRenderRenderFeatureRegistration}))
        {
            return 3;
        }
        auto messages = object::ObjectMessageQueue::create(256);
        if (!messages)
        {
            return 3;
        }
        auto& execution = (*engine)->execution();
        commands::CommandRegistry commands;
        commands::CommandDispatcher dispatcher(commands);
        bool closing{}, open_requested{};
        project::ProjectCreationOptions creation_options{installation, true};
        services::ServiceRegistry services(messages->dispatcherRef());
        auto scope = services.createScope();
        if (!scope || !scope->provide(services::ServiceNameView{"lux.process.execution"}, execution) ||
            !scope->provide(services::ServiceNameView{"lux.editor.project.creation.options"}, creation_options) ||
            !scope->provide(services::ServiceNameView{"lux.editor.installation"}, creation_options.installation) ||
            !services.publish(
                {services::ServiceEntry::bind<project::kProjectCreationService>(object::CodeLease::builtin()),
                 services::ServiceEntry::bind<kProjectLaunchingService>(object::CodeLease::builtin())}
            ))
        {
            return 3;
        }
        auto retire = [&]() noexcept
        {
            // Later locals release UI/shared owners first, including startup failure paths.
            // Accepted work is settled by the loop; only dispatcher reclamation remains here.
            if (!scope->release())
            {
                std::terminate();
            }
            while (!scope->drained())
            {
                if (!messages->collectRetired())
                {
                    std::terminate();
                }
            }
        };
        const cxx::scope_exit retire_services{retire};
        std::shared_ptr<ProjectLaunching> launching;
        auto creation = services.get<project::ProjectCreation>(*scope);
        if (!creation)
        {
            return 3;
        }
        desktop::UiRegistry windows(messages->dispatcherRef(), services);
        auto factories = desktop::UiCatalog::prepare(
            {desktop::UiEntry::bind<project::kProjectCreationView>(object::CodeLease::builtin())}
        );
        if (!factories || !windows.publish(*factories))
        {
            return 3;
        }
        auto desktop = desktop::DesktopShell::create(
            messages->dispatcherRef(),
            execution,
            (*engine)->sceneRuntime(),
            rendering.runtime(),
            rendering.resources(),
            &window
        );
        if (!desktop)
        {
            return 3;
        }
        auto factory = factories->find(project::kProjectCreationView.type);
        if (!factory)
        {
            return 3;
        }
        auto candidate = windows.create(
            *factory,
            *scope,
            {messages->dispatcherRef(),
             lux::ui::PaneId{"project-creation"},
             {},
             {factory->descriptor().schema, {}},
             views::ViewRestoreKey{"project-creation"}}
        );
        if (!candidate)
        {
            return 3;
        }
        (*candidate)->setModal(false);
        auto* pane = candidate->get();
        if (!(*desktop)->root().addSubPane(std::move(*candidate)))
        {
            return 3;
        }
        auto identity = (*desktop)->root().identify(*pane);
        if (!identity)
        {
            return 3;
        }
        std::vector<std::shared_ptr<commands::CommandEntry>> entries;
        entries.push_back(commands::CommandEntry::bind<command_lux_project_open>(
            lux::object::CodeLease::builtin(),
            [&](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{!closing && (!launching || !launching->pending())}; },
            [&](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt>
            {
                open_requested = true;
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        auto catalog = commands::CommandRegistrySnapshot::create(std::move(entries));
        if (!catalog || !commands.publish(std::move(*catalog)))
        {
            return 3;
        }
        if (!(*desktop)->installCommands(
                commands,
                dispatcher,
                [](const auto&, auto*, auto*) -> commands::CommandResult<commands::CommandInvocation>
                { return commands::CommandInvocation{}; }
            ))
        {
            return 3;
        }
        if (!(*creation)->start())
        {
            return 3;
        }
        window.on_close = [&](const window::WindowCloseEvent&) { closing = true; };
        input::Input input;
        int outcome{};
        const auto started = std::chrono::steady_clock::now();
        auto fail = [&](std::string_view operation)
        {
            std::fprintf(stderr, "launcher.%.*s failed\n", static_cast<int>(operation.size()), operation.data());
            outcome = 3;
            closing = true;
        };
        for (;;)
        {
            window::LuxWindow::pollEvents();
            if (!execution.collectCompletions())
            {
                fail("completions");
            }
            if (!execution.dispatchTaskEvents())
            {
                fail("tasks");
            }
            (void)messages->dispatchPending();
            if (!scope->maintain())
            {
                fail("services");
            }
            if (launching)
            {
                if (const auto* launched = launching->result())
                {
                    if (!*launched)
                    {
                        fail(launched->error().domain);
                    }
                    else
                    {
                        closing = true;
                    }
                    if (!launching->acknowledge())
                    {
                        fail("launch.acknowledge");
                    }
                }
            }
            if ((*creation)->progress().launched)
            {
                closing = true;
            }
            auto current = (*desktop)->root().findPane(*identity);
            if (!current)
            {
                fail("close");
            }
            else if ((*current)->hasCloseRequest())
            {
                closing = true;
            }
            if (closing)
            {
                window.hide(true);
                (*creation)->cancel();
                if (!(*creation)->progress().pending && (!launching || !launching->pending()))
                {
                    break;
                }
            }
            else
            {
                input.sample(window);
                if (!(*desktop)->feedInput(input.snapshot()))
                {
                    fail("input");
                }
            }
            if (!(*desktop)->update(closing ? std::optional{lux::ui::FrameInfo{}} : std::nullopt))
            {
                fail("desktop");
            }
            for (const auto& completion : (*desktop)->commands()->takeCompletions())
            {
                if (!completion.result)
                {
                    fail(completion.result.error().domain);
                }
            }
            if (!closing && std::exchange(open_requested, false))
            {
                const std::array filters{window::FileDialogFilter{"Lux project", "luxproject"}};
                auto chosen = window::openFileDialog(&window, filters);
                if (!chosen)
                {
                    fail("dialog");
                }
                else if (*chosen)
                {
                    if (!launching)
                    {
                        auto service = services.get<ProjectLaunching>(*scope);
                        if (!service)
                        {
                            fail("launch.service");
                        }
                        else
                        {
                            launching = std::move(*service);
                        }
                    }
                    if (launching && !launching->request(std::move(**chosen)))
                    {
                        fail("launch.request");
                    }
                }
            }
            auto driven = (*engine)->sceneRuntime().driveFrame();
            if (!driven || !driven->empty())
            {
                fail("scenes");
            }
            if (smoke_frames && (*desktop)->presentation().capturedFrames() >= smoke_frames)
            {
                closing = true;
            }
            if (smoke_frames && std::chrono::steady_clock::now() - started > std::chrono::seconds(30))
            {
                fail("smoke.timeout");
            }
            window::LuxWindow::waitEvents(0.001);
        }
        return outcome;
    }
} // namespace lux::editor::application
