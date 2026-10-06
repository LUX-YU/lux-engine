#include <lux/engine/editor/sessions/SessionServices.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>

namespace lux::editor::sessions
{
    namespace
    {
        constexpr services::ServiceContract store_contracts[]{
            services::ServiceContract::forType<SessionStore, SessionStore>(
                services::ServiceNameView{"lux.editor.sessions"}
            )
        };
        constexpr services::ServiceContract opening_contracts[]{
            services::ServiceContract::forType<SessionOpening, SessionOpening>(
                services::ServiceNameView{"lux.editor.sessions.opening"}
            )
        };
        constexpr services::ServiceDependency opening_dependencies[]{
            {services::ServiceNameView{"lux.process.execution"}, 1, cxx::typeToken<process::ExecutionRuntime>(),
             services::EDependencyKind::BORROWED, services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.sessions"}, 1, cxx::typeToken<SessionStore>(),
             services::EDependencyKind::SHARED, services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.saves"}, 1, cxx::typeToken<persistence::SaveService>(),
             services::EDependencyKind::SHARED, services::EDependencyScope::ROOT},
            // Opening is an existing factory boundary: each selected content descriptor carries its own
            // dependency list. Only that list is passed to withDependencies after admission and deduplication.
            {services::ServiceNameView{"lux.services.registry"}, 1, cxx::typeToken<services::ServiceRegistry>(),
             services::EDependencyKind::BORROWED, services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.services.scope"}, 1, cxx::typeToken<services::ServiceScope>(),
             services::EDependencyKind::BORROWED, services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<SessionStore>>
        createStore(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            return std::make_unique<SessionStore>(resolver.dispatcher(), 128);
        }
        services::ServiceResult<std::unique_ptr<SessionOpening>>
        createOpening(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto runtime = resolver.require<process::ExecutionRuntime>(0);
            if (!runtime)
                return cxx::unexpected(std::move(runtime.error()));
            auto store = resolver.get<SessionStore>(1);
            if (!store)
                return cxx::unexpected(std::move(store.error()));
            auto saves = resolver.get<persistence::SaveService>(2);
            if (!saves)
                return cxx::unexpected(std::move(saves.error()));
            auto registry = resolver.require<services::ServiceRegistry>(3);
            if (!registry)
                return cxx::unexpected(std::move(registry.error()));
            auto scope = resolver.require<services::ServiceScope>(4);
            if (!scope)
                return cxx::unexpected(std::move(scope.error()));
            return std::make_unique<SessionOpening>(
                runtime->get(), std::move(*store), std::move(*saves), registry->get(), scope->get()
            );
        }
    } // namespace
    constinit const services::ServiceDescriptor kSessionStoreService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<SessionStore, createStore>(
            services::ServiceNameView{"lux.editor.sessions"}, store_contracts
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        return descriptor;
    }();
    constinit const services::ServiceDescriptor kSessionOpeningService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<SessionOpening, createOpening>(
            services::ServiceNameView{"lux.editor.sessions.opening"}, opening_contracts, opening_dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.maintain = [](void* allocation) noexcept -> services::ServiceResult<void>
        {
            auto updated = static_cast<SessionOpening*>(allocation)->update();
            if (!updated)
            {
                auto& error = updated.error();
                const auto code = error.code == ESessionFactoryError::BUSY ? services::EServiceError::BUSY
                                                                         : services::EServiceError::FACTORY_FAILURE;
                return cxx::unexpected(services::ServiceFailure{
                    code, std::move(error.detail), "sessions.opening", static_cast<std::uint64_t>(error.code)
                });
            }
            return {};
        };
        descriptor.settled = [](const void* allocation) noexcept -> services::ServiceResult<bool>
        {
            return static_cast<const SessionOpening*>(allocation)->settled();
        };
        return descriptor;
    }();
} // namespace lux::editor::sessions
