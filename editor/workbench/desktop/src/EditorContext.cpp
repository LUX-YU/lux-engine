#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/desktop/EditorContext.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>

namespace lux::editor::desktop
{
    struct EditorContext::Impl final
    {
        services::ServiceRegistry services;
        commands::CommandRegistry commands;
        UiRegistry ui;
        explicit Impl(object::ObjectDispatcherRef dispatcher)
            : services(dispatcher), ui(std::move(dispatcher), services)
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
    UiRegistry& EditorContext::ui() noexcept
    {
        return impl_->ui;
    }
    commands::CommandRegistry& EditorContext::commands() noexcept
    {
        return impl_->commands;
    }
} // namespace lux::editor::desktop
