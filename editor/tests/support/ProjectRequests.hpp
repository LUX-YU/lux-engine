#pragma once
#include <algorithm>
#include <cassert>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Command.hpp>

namespace fixture
{
    inline bool preparationsFinished(lux::editor::LuxEngine& host)
    {
        const auto infos = host.engine().execution().taskInfos();
        return std::ranges::none_of(
            infos,
            [](const auto& info)
            {
                const bool active =
                    info.state == lux::process::ETaskState::QUEUED || info.state == lux::process::ETaskState::RUNNING;
                return info.category == "editor.project" && active;
            }
        );
    }

    inline lux::editor::FrameworkResult<void> open(lux::editor::LuxEngine& host, std::filesystem::path file)
    {
        lux::editor::OpenProjectRequest request{std::move(file)};
        assert(lux::object::sendEvent(host, request));
        if (request.rejection.type)
        {
            return lux::cxx::unexpected(request.rejection);
        }
        return {};
    }

    inline lux::editor::FrameworkResult<void> create(
        lux::editor::LuxEngine& host,
        lux::editor::CreateProjectRequest request
    )
    {
        assert(lux::object::sendEvent(host, request));
        if (request.rejection.type)
        {
            return lux::cxx::unexpected(request.rejection);
        }
        return {};
    }

    inline void close(lux::editor::LuxEngine& host) noexcept
    {
        lux::editor::CloseProjectRequest request;
        assert(lux::object::sendEvent(host, request));
    }

    inline void cancel(lux::editor::LuxEngine& host) noexcept
    {
        lux::ui::Command command{lux::ui::CommandIdView{"lux.project.cancel_open"}, lux::ui::ECommandPhase::EXECUTE};
        assert(lux::object::sendEvent(host, command));
        assert(command.result == lux::ui::ECommandDispatchResult::EXECUTED);
    }

    struct ProjectFacts final
    {
        explicit ProjectFacts(lux::editor::LuxEngine& host)
        {
            changed_connection = lux::object::LuxObject::connect(
                &host,
                &lux::editor::LuxEngine::projectChanged,
                [this]() noexcept { ++changed; }
            );
            failed_connection = lux::object::LuxObject::connect(
                &host,
                &lux::editor::LuxEngine::projectOpenFailed,
                [this](const lux::editor::ProjectOpenFailure& value) noexcept
                {
                    ++failed;
                    failure = value;
                }
            );
            assert(changed_connection && failed_connection);
        }

        unsigned changed{}, failed{};
        lux::editor::ProjectOpenFailure failure;
        lux::object::LuxObject::ConnectResult changed_connection, failed_connection;
    };
} // namespace fixture
