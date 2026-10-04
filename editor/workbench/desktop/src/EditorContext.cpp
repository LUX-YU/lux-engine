#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/desktop/EditorContext.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>

namespace lux::editor::desktop
{
    namespace
    {
        services::ServiceScope initialScope(services::ServiceRegistry& registry)
        {
            // This is the first scope of the private registry with its fixed nonzero default limits.
            auto scope = registry.createScope();
            if (!scope)
            {
                std::terminate();
            }
            return std::move(*scope);
        }
    } // namespace
    struct EditorContext::Impl final
    {
        services::ServiceRegistry services;
        services::ServiceScope scope;
        commands::CommandRegistry commands;
        UiRegistry ui;
        explicit Impl(object::ObjectDispatcherRef dispatcher)
            : services(dispatcher), scope(initialScope(services)), commands(services, scope),
              ui(std::move(dispatcher), services)
        {
        }
    };
    EditorContext::EditorContext(object::ObjectDispatcherRef dispatcher)
        : impl_(std::make_unique<Impl>(std::move(dispatcher)))
    {
    }
    EditorContext::~EditorContext() = default;
    services::ServiceRegistry& EditorContext::services() noexcept
    {
        return impl_->services;
    }
    services::ServiceScope& EditorContext::scope() noexcept
    {
        return impl_->scope;
    }
    UiRegistry& EditorContext::ui() noexcept
    {
        return impl_->ui;
    }
    commands::CommandRegistry& EditorContext::commands() noexcept
    {
        return impl_->commands;
    }
} // namespace lux::editor::desktop
