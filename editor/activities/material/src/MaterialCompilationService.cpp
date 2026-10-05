#include <algorithm>
#include <lux/engine/editor/material/MaterialCompilationService.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>

namespace lux::editor::material
{
    namespace
    {
        constexpr services::ServiceContract compilation_contracts[]{
            services::ServiceContract::forType<MaterialCompilationService, MaterialCompilationService>(
                services::ServiceNameView{"lux.editor.material.compilation"}
            )
        };
        constexpr services::ServiceDependency compilation_dependencies[]{
            {services::ServiceNameView{"lux.process.execution"},
             1,
             cxx::typeToken<process::ExecutionRuntime>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<MaterialCompilationService>>
        createCompilation(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto execution = resolver.require<process::ExecutionRuntime>(0);
            if (!execution)
            {
                return cxx::unexpected(std::move(execution.error()));
            }
            return std::make_unique<MaterialCompilationService>(execution->get());
        }
        auto rejected(EMaterialCompileRequestError error)
        {
            return cxx::unexpected(VMaterialCompileFailure{error});
        }
    } // namespace
    constinit const services::ServiceDescriptor kMaterialCompilationService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<MaterialCompilationService, createCompilation>(
            services::ServiceNameView{"lux.editor.material.compilation"},
            compilation_contracts,
            compilation_dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.settled = [](const void* allocation) noexcept -> services::ServiceResult<bool>
        { return static_cast<const MaterialCompilationService*>(allocation)->settled(); };
        return descriptor;
    }();

    MaterialCompilationService::MaterialCompilationService(process::ExecutionRuntime& runtime, std::size_t capacity)
        : runtime_(runtime), capacity_(capacity)
    {
        operations_.reserve(capacity);
    }
    MaterialCompilationService::~MaterialCompilationService() = default;
    MaterialCompileResult<std::vector<MaterialCompileId>> MaterialCompilationService::snapshotIds() const
    {
        if (owner_ != std::this_thread::get_id())
        {
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        }
        std::vector<MaterialCompileId> result;
        result.reserve(operations_.size());
        for (const auto& operation : operations_)
        {
            result.push_back(operation.operation->id());
        }
        return result;
    }
    MaterialCompileResult<MaterialCompileId> MaterialCompilationService::start(
        MaterialSnapshot snapshot,
        MaterialCompileSettings settings,
        std::uint64_t environment,
        lux::scene::RenderAssetInput assets
    )
    {
        if (owner_ != std::this_thread::get_id())
        {
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        }
        if (operations_.size() == capacity_)
        {
            return rejected(EMaterialCompileRequestError::CAPACITY);
        }
        auto operation = MaterialCompileOperation::start(runtime_, std::move(snapshot), settings, environment);
        if (!operation)
        {
            return cxx::unexpected(operation.error());
        }
        const auto id = (*operation)->id();
        operations_.push_back({std::move(*operation), std::move(assets)});
        return id;
    }
    MaterialCompileResult<std::reference_wrapper<const MaterialCompileOperation>> MaterialCompilationService::operation(
        MaterialCompileId id
    ) const noexcept
    {
        if (owner_ != std::this_thread::get_id())
        {
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        }
        const auto found =
            std::ranges::find_if(operations_, [id](const auto& value) { return value.operation->id() == id; });
        if (found == operations_.end())
        {
            return rejected(EMaterialCompileRequestError::INVALID_ID);
        }
        return std::cref(*found->operation);
    }
    MaterialCompileResult<void> MaterialCompilationService::acknowledge(MaterialCompileId id)
    {
        if (owner_ != std::this_thread::get_id())
        {
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        }
        const auto found =
            std::ranges::find_if(operations_, [id](const auto& value) { return value.operation->id() == id; });
        if (found == operations_.end())
        {
            return rejected(EMaterialCompileRequestError::INVALID_ID);
        }
        if (!found->operation->ready())
        {
            return rejected(EMaterialCompileRequestError::BUSY);
        }
        auto retiring = std::move(*found);
        operations_.erase(found); // Erase before callbacks can reenter through payload/code destruction.
        return {};
    }
    MaterialCompileResult<void> MaterialCompilationService::cancel(MaterialCompileId id) noexcept
    {
        if (owner_ != std::this_thread::get_id())
        {
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        }
        const auto found =
            std::ranges::find_if(operations_, [id](const auto& value) { return value.operation->id() == id; });
        if (found == operations_.end())
        {
            return rejected(EMaterialCompileRequestError::INVALID_ID);
        }
        found->operation->cancel();
        return {};
    }
    MaterialCompileResult<std::optional<MaterialCompileId>> MaterialCompilationService::latest(
        sessions::SessionId session
    ) const noexcept
    {
        if (owner_ != std::this_thread::get_id())
        {
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        }
        if (!session.valid())
        {
            return rejected(EMaterialCompileRequestError::INVALID_ID);
        }
        for (auto record = operations_.rbegin(); record != operations_.rend(); ++record)
        {
            if (record->operation->key().content.session == session)
            {
                return std::optional{record->operation->id()};
            }
        }
        return std::optional<MaterialCompileId>{};
    }
    MaterialCompileResult<lux::scene::RenderAssetInput> MaterialCompilationService::assets(MaterialCompileId id
    ) const noexcept
    {
        if (owner_ != std::this_thread::get_id())
        {
            return rejected(EMaterialCompileRequestError::WRONG_THREAD);
        }
        const auto found =
            std::ranges::find_if(operations_, [id](const auto& value) { return value.operation->id() == id; });
        if (found == operations_.end())
        {
            return rejected(EMaterialCompileRequestError::INVALID_ID);
        }
        return found->assets;
    }
    bool MaterialCompilationService::settled() const noexcept
    {
        return std::ranges::all_of(operations_, [](const auto& record) { return record.operation->ready(); });
    }
    bool MaterialCompilationService::empty() const noexcept
    {
        return operations_.empty();
    }

} // namespace lux::editor::material
