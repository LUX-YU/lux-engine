#include <lux/engine/editor/detail/HistoryCommandBinding.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>

namespace lux::editor::sessions
{
    namespace
    {
        constexpr services::ServiceDependency dependencies[]{
            {services::ServiceNameView{"lux.editor.sessions"}, 1, cxx::typeToken<SessionStore>(),
             services::EDependencyKind::SHARED, services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.sessions.opening"}, 1, cxx::typeToken<SessionOpening>(),
             services::EDependencyKind::SHARED, services::EDependencyScope::ROOT}
        };
        auto bindingFailure(services::ServiceFailure error)
        {
            const auto value = factoryFailure(std::move(error));
            auto code = commands::ECommandError::DOMAIN_FAILURE;
            if (value.code == ESessionFactoryError::BUSY)
            {
                code = commands::ECommandError::BUSY;
            }
            else if (value.code == ESessionFactoryError::CLOSED)
            {
                code = commands::ECommandError::CLOSED;
            }
            return cxx::unexpected(commands::CommandFailure{code, value.domain, value.domain_code, value.detail});
        }
        template <bool Forward>
        commands::CommandResult<std::unique_ptr<commands::CommandBinding>> bindHistory(
            services::ServiceResolver& resolver, const object::CodeLease&
        ) noexcept
        {
            auto store = resolver.get<SessionStore>(0);
            if (!store)
            {
                return bindingFailure(std::move(store.error()));
            }
            auto opening = resolver.get<SessionOpening>(1);
            if (!opening)
            {
                return bindingFailure(std::move(opening.error()));
            }
            auto roles = std::make_shared<HistoryActionLookup>(
                [content = *store, owner = *opening](SessionId id) noexcept
                {
                    // Both exact providers outlive the binding, including command dispatch cleanup.
                    return owner->find(id);
                }
            );
            return detail::makeHistoryBinding(**store, std::move(roles), Forward);
        }
        constexpr commands::CommandDescriptor undo = []
        {
            auto descriptor = kUndoCommand;
            descriptor.dependencies = dependencies;
            descriptor.create = bindHistory<false>;
            return descriptor;
        }();
        constexpr commands::CommandDescriptor redo = []
        {
            auto descriptor = kRedoCommand;
            descriptor.dependencies = dependencies;
            descriptor.create = bindHistory<true>;
            return descriptor;
        }();
    } // namespace
    std::vector<std::shared_ptr<commands::CommandEntry>> makeHistoryCommands(object::CodeLease code)
    {
        return {commands::CommandEntry::bind<undo>(code), commands::CommandEntry::bind<redo>(std::move(code))};
    }
} // namespace lux::editor::sessions
