#pragma once
#include <algorithm>
#include <cassert>
#include <chrono>
#include <lux/engine/Engine.hpp>
#include <lux/engine/editor/EditorApplication.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Command.hpp>
#include <thread>

namespace fixture
{
    inline lux::editor::EditorConfig loadedConfig(std::string title, int width, int height, bool vsync = true)
    {
        using namespace lux::editor;
        const auto directory = std::filesystem::temp_directory_path() / "lux-editor-session-fixtures";
        std::filesystem::create_directories(directory);
        auto file = directory / (title + ".luxproj");
        ProjectManifest manifest{1, *uuids::uuid::from_string("189c1234-0000-4000-8000-000000000001"), title};
        assert(writeProjectManifestAtomic(file, manifest, EProjectWrite::REPLACE));
        EditorConfig config{std::move(title), width, height, vsync};
        config.project_file = std::move(file);
        return config;
    }

    inline lux::editor::FrameworkResult<bool> stepEditor(lux::editor::EditorApplication& application) noexcept
    {
        auto step = application.engine().step();
        if (!step)
        {
            return lux::cxx::unexpected(step.error());
        }
        if (*step == lux::engine::EEngineProgress::STOPPED)
        {
            auto finished = application.run();
            if (!finished)
            {
                return lux::cxx::unexpected(finished.error());
            }
            return false;
        }
        return true;
    }

    inline lux::editor::FrameworkResult<std::unique_ptr<lux::editor::EditorApplication>> createLoadedEditor(
        lux::editor::EditorConfig config,
        lux::editor::EditorAssembly assembly = {},
        lux::editor::ProjectStartup startup = {}
    )
    {
        auto application =
            lux::editor::EditorApplication::create(std::move(config), std::move(assembly), std::move(startup));
        if (!application)
        {
            return lux::cxx::unexpected(application.error());
        }
        lux::error::Error failure;
        auto completion = lux::object::LuxObject::connect(
            application->get(),
            &lux::editor::EditorApplication::projectLoadCompleted,
            [&failure](const lux::editor::ProjectLoadOutcome& value) noexcept { failure = value.error; }
        );
        assert(completion);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!(*application)->ready() && !failure.type)
        {
            auto advanced = (*application)->engine().step();
            if (!advanced)
            {
                return lux::cxx::unexpected(advanced.error());
            }
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        if (failure.type)
        {
            (*application)->engine().requestClose();
            static_cast<void>((*application)->run());
            return lux::cxx::unexpected(failure);
        }
        return std::move(*application);
    }

    inline bool preparationsFinished(lux::editor::EditorApplication& host)
    {
        const auto infos = host.engine().execution().taskInfos();
        return std::ranges::none_of(
            infos,
            [](const auto& info)
            {
                const bool active = info.state == lux::execution::ETaskState::QUEUED ||
                                    info.state == lux::execution::ETaskState::RUNNING;
                return info.category == "editor.project" && active;
            }
        );
    }

    inline lux::editor::FrameworkResult<void> open(lux::editor::EditorApplication& host, std::filesystem::path file)
    {
        lux::editor::OpenProjectRequest request{std::move(file)};
        assert(lux::object::sendEvent(host, request));
        if (request.rejection.type)
        {
            return lux::cxx::unexpected(request.rejection);
        }
        return {};
    }

    inline lux::editor::FrameworkResult<void>
    create(lux::editor::EditorApplication& host, lux::editor::CreateProjectRequest request)
    {
        assert(lux::object::sendEvent(host, request));
        if (request.rejection.type)
        {
            return lux::cxx::unexpected(request.rejection);
        }
        return {};
    }

    inline void close(lux::editor::EditorApplication& host) noexcept
    {
        lux::editor::CloseProjectRequest request;
        assert(lux::object::sendEvent(host, request));
    }

    inline void cancel(lux::editor::EditorApplication& host) noexcept
    {
        lux::ui::Command command{lux::ui::CommandIdView{"lux.project.cancel_open"}, lux::ui::ECommandPhase::EXECUTE};
        assert(lux::object::sendEvent(host, command));
        assert(command.result == lux::ui::ECommandDispatchResult::EXECUTED);
    }

    struct ProjectFacts final
    {
        explicit ProjectFacts(lux::editor::EditorApplication& host)
        {
            changed_connection = lux::object::LuxObject::connect(
                &host,
                &lux::editor::EditorApplication::projectChanged,
                [this]() noexcept { ++changed; }
            );
            failed_connection = lux::object::LuxObject::connect(
                &host,
                &lux::editor::EditorApplication::projectLoadCompleted,
                [this](const lux::editor::ProjectLoadOutcome& value) noexcept
                {
                    ++completed;
                    if (value.error.type == lux::editor::Errors::ProjectCancelled)
                    {
                        ++cancelled;
                    }
                    else if (value.error.type)
                    {
                        ++failed;
                        failure = value;
                    }
                }
            );
            assert(changed_connection && failed_connection);
        }

        unsigned changed{}, failed{}, completed{}, cancelled{};
        lux::editor::ProjectLoadOutcome failure;
        lux::object::LuxObject::ConnectResult changed_connection, failed_connection;
    };
} // namespace fixture
