#include <lux/engine/editor/persistence/PersistenceServices.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>

namespace lux::editor::persistence
{
    namespace
    {
        constexpr services::ServiceContract write_contracts[]{
            services::ServiceContract::forType<WriteCoordinator, WriteCoordinator>(
                services::ServiceNameView{"lux.editor.persistence.writes"}
            )
        };
        constexpr services::ServiceContract save_contracts[]{
            services::ServiceContract::forType<SaveService, SaveService>(
                services::ServiceNameView{"lux.editor.persistence.saves"}
            )
        };
        constexpr services::ServiceContract execution_contracts[]{
            services::ServiceContract::forType<SaveExecution, SaveExecution>(
                services::ServiceNameView{"lux.editor.persistence.execution"}
            )
        };
        constexpr services::ServiceDependency save_dependencies[]{
            {services::ServiceNameView{"lux.editor.persistence.writes"}, 1, cxx::typeToken<WriteCoordinator>(),
             services::EDependencyKind::SHARED, services::EDependencyScope::ROOT}
        };
        constexpr services::ServiceDependency execution_dependencies[]{
            {services::ServiceNameView{"lux.process.execution"}, 1, cxx::typeToken<process::ExecutionRuntime>(),
             services::EDependencyKind::BORROWED, services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.saves"}, 1, cxx::typeToken<SaveService>(),
             services::EDependencyKind::SHARED, services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.writes"}, 1, cxx::typeToken<WriteCoordinator>(),
             services::EDependencyKind::SHARED, services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.files"}, 1, cxx::typeToken<IArtifactStore>(),
             services::EDependencyKind::SHARED, services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<WriteCoordinator>>
        createWrites(services::ServiceResolver&, const services::ServiceConfiguration&) noexcept
        {
            return std::make_unique<WriteCoordinator>();
        }
        services::ServiceResult<std::unique_ptr<SaveService>>
        createSaves(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto writes = resolver.get<WriteCoordinator>(0);
            if (!writes)
            {
                return cxx::unexpected(std::move(writes.error()));
            }
            return std::make_unique<SaveService>(std::move(*writes));
        }
        services::ServiceResult<std::unique_ptr<SaveExecution>>
        createExecution(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto runtime = resolver.require<process::ExecutionRuntime>(0);
            if (!runtime)
            {
                return cxx::unexpected(std::move(runtime.error()));
            }
            auto saves = resolver.get<SaveService>(1);
            if (!saves)
            {
                return cxx::unexpected(std::move(saves.error()));
            }
            auto writes = resolver.get<WriteCoordinator>(2);
            if (!writes)
            {
                return cxx::unexpected(std::move(writes.error()));
            }
            auto files = resolver.get<IArtifactStore>(3);
            if (!files)
            {
                return cxx::unexpected(std::move(files.error()));
            }
            return std::make_unique<SaveExecution>(
                runtime->get(), std::move(*saves), std::move(*writes), std::move(*files)
            );
        }
    } // namespace

    constinit const services::ServiceDescriptor kWriteCoordinatorService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<WriteCoordinator, createWrites>(
            services::ServiceNameView{"lux.editor.persistence.writes"}, write_contracts
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        return descriptor;
    }();
    constinit const services::ServiceDescriptor kSaveService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<SaveService, createSaves>(
            services::ServiceNameView{"lux.editor.persistence.saves"}, save_contracts, save_dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.maintain = [](void* allocation) noexcept -> services::ServiceResult<void>
        {
            static_cast<SaveService*>(allocation)->adoptCompletions();
            return {};
        };
        return descriptor;
    }();
    constinit const services::ServiceDescriptor kSaveExecutionService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<SaveExecution, createExecution>(
            services::ServiceNameView{"lux.editor.persistence.execution"}, execution_contracts, execution_dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.maintain = [](void* allocation) noexcept -> services::ServiceResult<void>
        {
            auto submitted = static_cast<SaveExecution*>(allocation)->submitReady();
            if (!submitted)
            {
                auto& error = submitted.error();
                const auto code = error.code == EPersistenceError::BUSY ? services::EServiceError::BUSY
                                                                      : services::EServiceError::FACTORY_FAILURE;
                return cxx::unexpected(services::ServiceFailure{
                    code, std::move(error.detail), "persistence", static_cast<std::uint64_t>(error.code)
                });
            }
            return {};
        };
        return descriptor;
    }();
} // namespace lux::editor::persistence
