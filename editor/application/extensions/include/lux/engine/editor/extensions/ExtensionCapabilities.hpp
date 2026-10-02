#pragma once

#include <lux/engine/object/ObjectDispatcher.hpp>

namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::editor
{
    class ProjectStorage;
    namespace sessions
    {
        class SessionStore;
    }
    namespace persistence
    {
        class SaveService;
        class WriteCoordinator;
    }
    namespace desktop
    {
        class ViewHost;
    }
    namespace commands
    {
        class CommandRegistry;
    }
}

namespace lux::editor::extensions
{
    struct SessionActivities final
    {
        sessions::SessionStore& sessions;
        persistence::SaveService& saves;
    };
    struct ProjectActivities final
    {
        ProjectStorage& project;
        persistence::WriteCoordinator& writes;
        process::ExecutionRuntime& execution;
    };
    struct WorkbenchAccess final
    {
        object::ObjectDispatcherRef dispatcher;
        desktop::ViewHost& views;
        commands::CommandRegistry& commands;
    };
    struct ExtensionRequirements final
    {
        bool sessions{}, project{}, workbench{};
    };
    // Callback-only aggregate. Capture the required provider references, never these group pointers.
    // Providers outlive accepted work, mounted views and contribution closures. Activation may prepare
    // closures/owners; work starts only when its actual owner has been installed. A plugin owns its
    // Connections and TaskScopes; it never installs a second provider or a private message pump.
    struct ExtensionCapabilities final
    {
        const SessionActivities* sessions{};
        const ProjectActivities* project{};
        const WorkbenchAccess* workbench{};
    };
}
