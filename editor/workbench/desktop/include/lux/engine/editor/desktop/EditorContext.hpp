#pragma once

#include <lux/engine/object/ObjectDispatcher.hpp>
#include <memory>

namespace lux::services
{
    class ServiceRegistry;
    class ServiceScope;
} // namespace lux::services
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
        [[nodiscard]] services::ServiceScope& scope() noexcept;
        [[nodiscard]] UiRegistry& ui() noexcept;
        [[nodiscard]] commands::CommandRegistry& commands() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::desktop
