#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/ProjectCreationPane.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/editor/detail/EditorStartup.hpp>
#include <lux/engine/editor/detail/ProjectPreparation.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor::detail
{
    namespace
    {
        struct InitialProject final
        {
            std::filesystem::path file;
            std::optional<ProjectManifest> create;
            std::optional<ProjectPreparation> prepared;
            bool requested{};
            bool pending{};
        };

        void openProject(void* value, OpenProjectRequest& request) noexcept
        {
            auto& selection = *static_cast<InitialProject*>(value);
            selection.file = request.manifest_file;
            selection.create.reset();
            selection.requested = true;
        }

        void createProject(void* value, CreateProjectRequest& request) noexcept
        {
            auto& selection = *static_cast<InitialProject*>(value);
            selection.file = request.root / "Project.luxproj";
            selection.create.emplace(std::move(request.manifest));
            selection.requested = true;
        }

        FrameworkResult<void> startPreparation(
            execution::ExecutionRuntime& execution,
            InitialProject& selection,
            const EditorConfig& config
        ) noexcept
        {
            auto blocking = execution.blocking();
            if (!blocking)
            {
                return cxx::unexpected(execution::toError(blocking.error()));
            }
            selection.requested = false;
            selection.prepared.reset();
            selection.pending = true;
            auto work =
                stdexec::schedule(*blocking) |
                stdexec::then([&selection, &config]() noexcept
                              { return prepareProject(selection.file, selection.create, config.plugin_locations, {}); }
                );
            auto normalized =
                stdexec::upon_error(
                    std::move(work),
                    [](execution::EExecutionError error) noexcept
                    { return ProjectPreparation{cxx::unexpected(execution::toError(error)), false}; }
                ) |
                stdexec::upon_stopped(
                    []() noexcept
                    { return ProjectPreparation{cxx::unexpected(error::Error{Errors::ProjectCancelled}), false}; }
                );
            auto admitted = execution.spawn(stdexec::then(
                stdexec::continues_on(std::move(normalized), execution.mainScheduler()),
                [&selection](ProjectPreparation value) noexcept
                {
                    selection.prepared.emplace(std::move(value));
                    selection.pending = false;
                }
            ));
            if (!admitted)
            {
                selection.pending = false;
                return cxx::unexpected(execution::toError(admitted.error()));
            }
            return {};
        }
    } // namespace

    FrameworkResult<PreparedProject> prepareInitialProject(
        execution::ExecutionRuntime& execution,
        EditorWindow& window,
        const EditorConfig& config,
        ProjectStartupRequest request,
        StartupPump pump
    ) noexcept
    {
        InitialProject selection{config.project_file, std::move(request.create), {}, !config.project_file.empty()};
        ProjectCreationPane* chooser = nullptr;
        if (config.project_file.empty())
        {
            auto pane = std::make_unique<ProjectCreationPane>(
                &selection,
                &openProject,
                &createProject,
                std::move(request.available_plugins)
            );
            chooser = pane.get();
            if (auto added = window.uiRoot().addPane(std::move(pane)); !added)
            {
                return cxx::unexpected(error::Error{Errors::ProjectCancelled});
            }
        }
        cxx::scope_exit remove_chooser(
            [&]() noexcept
            {
                if (chooser && !window.uiRoot().removePane(*chooser))
                {
                    std::terminate(); // Startup returns outside Root traversal and all preparation callbacks.
                }
            }
        );
        error::Error failure;
        for (;;)
        {
            if (selection.requested && !selection.pending && !window.shouldClose())
            {
                if (auto started = startPreparation(execution, selection, config); !started)
                {
                    return cxx::unexpected(started.error());
                }
            }
            if (!selection.pending)
            {
                if (failure.type != 0)
                {
                    return cxx::unexpected(failure);
                }
                if (window.shouldClose())
                {
                    return cxx::unexpected(error::Error{Errors::ProjectCancelled});
                }
                if (selection.prepared)
                {
                    if (selection.prepared->result || !chooser)
                    {
                        return std::move(selection.prepared->result);
                    }
                    chooser->setStatus(error::format(selection.prepared->result.error()));
                    selection.prepared.reset();
                }
            }
            if (failure.type == 0)
            {
                const auto epoch = execution.wakeEpoch();
                auto driven = pump();
                if (!driven)
                {
                    failure = driven.error();
                }
                else
                {
                    const bool selection_ready =
                        selection.prepared.has_value() || selection.requested || window.shouldClose();
                    const bool execution_ready = execution.hasReadyWork() || execution.wakeEpoch() != epoch;
                    const bool should_wait = !*driven && !selection_ready && !execution_ready;
                    if (should_wait)
                    {
                        window::LuxWindow::waitEvents();
                    }
                }
            }
            else
            {
                // An accepted IO operation still borrows selection/config. Drain its actual completion before return.
                const auto epoch = execution.wakeEpoch();
                auto completed = execution.dispatchMainCompletions();
                if (!completed)
                {
                    std::terminate(); // Factory is Main-only, never entered from an Execution callback.
                }
                if (selection.pending)
                {
                    execution.waitForWork(epoch);
                }
            }
        }
    }
} // namespace lux::editor::detail
