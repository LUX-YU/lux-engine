#pragma once

#include <lux/engine/object/ObjectDispatcher.hpp>
#include <memory>

namespace lux::services
{
    class ServiceRegistry;
}
namespace lux::editor::commands
{
    class CommandRegistry;
}
namespace lux::editor::desktop
{
    class UiRegistry;

    // One composition environment, with no live tool state. Factories retain their exact dependencies,
    // not this environment. The dispatcher and explicitly provided infrastructure outlive all scopes.
    class EditorContext final
    {
    public:
        explicit EditorContext(object::ObjectDispatcherRef);
        ~EditorContext();
        EditorContext(const EditorContext&) = delete;
        EditorContext& operator=(const EditorContext&) = delete;
        EditorContext(EditorContext&&) = delete;
        EditorContext& operator=(EditorContext&&) = delete;

        [[nodiscard]] services::ServiceRegistry& services() noexcept;
        [[nodiscard]] UiRegistry& ui() noexcept;
        [[nodiscard]] commands::CommandRegistry& commands() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::desktop
