#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/runtime/BuiltinRegistration.hpp>
#include <lux/engine/render/runtime/commands/Registration.hpp>
#include <lux/engine/render/resource/instance/RasterMeshInstanceStorage.hpp>
#include <lux/engine/render/resource/lighting/DirectLightingStorage.hpp>
#include <lux/engine/render/resource/material/RasterMaterialStorage.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/texture/SampledTexture2DStorage.hpp>
#include <lux/engine/render/resource/transform/TransformStorage.hpp>
#include <lux/engine/render/resource/view/ViewParameterStorage.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <atomic>
#include <optional>

namespace lux::render
{
    RenderServer::RenderServer(std::unique_ptr<Impl> state) noexcept : impl_(std::move(state)) {}

    RenderServer::~RenderServer() noexcept
    {
        if (impl_->state_.load() == Impl::EState::RUNNING)
        {
            std::terminate(); // the application must stop and join its rendering thread
        }
        if (impl_->canAccessCatalog())
        {
            impl_->shutdown();
        }
    }

    RenderResult<std::unique_ptr<RenderServer>>
    RenderServer::create(RenderServerConfig config, execution::ExecutionRuntime& execution) noexcept
    {
        const bool invalid =
            config.frames_in_flight == 0 ||
            config.frames_in_flight > 3 ||
            config.max_config_bytes == 0 ||
            config.gpu_wait_ns > 1'000'000;

        if (invalid)
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }

        auto responder = ipc::IpcCore::instance().createResponder(config.channel_name);
        if (!responder)
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }

        static std::atomic<std::uint64_t> next_instance_id{1};
        auto next = next_instance_id.load(std::memory_order_relaxed);
        do
        {
            if (next == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
        } while (!next_instance_id.compare_exchange_weak(next, next + 1, std::memory_order_relaxed));

        auto state = std::make_unique<RenderServer::Impl>(
            ServerInstanceId{next}, std::move(config), std::move(*responder), execution
        );
        auto server = std::unique_ptr<RenderServer>(new RenderServer(std::move(state)));
        if (auto registered = server->registerBuiltins(); !registered)
        {
            return cxx::unexpected(registered.error());
        }

        return server;
    }

    RenderResult<void> RenderServer::registerBuiltins() noexcept
    {
        const std::array registrations{
            std::pair{&detail::ViewParameterStorage::factory, &detail::viewParameterStorageCommands},
            std::pair{&detail::SampledTexture2DStorage::factory, &detail::sampledTexture2DStorageCommands},
            std::pair{&detail::IndexedTriangleMeshStorage::factory, &detail::indexedTriangleMeshStorageCommands},
            std::pair{&detail::RasterMaterialStorage::factory, &detail::rasterMaterialStorageCommands},
            std::pair{&detail::RasterMeshInstanceStorage::factory, &detail::rasterMeshInstanceStorageCommands},
            std::pair{&detail::TransformStorage::factory, &detail::transformStorageCommands},
            std::pair{&detail::DirectLightingStorage::factory, &detail::directLightingStorageCommands}
        };
        for (const auto& [factory, commands] : registrations)
        {
            auto added = registerResource(factory(), {}, commands());
            if (!added) return cxx::unexpected(added.error());
        }
        if (auto depth = detail::registerDepthPyramid(*this); !depth) return depth;
        if (auto lighting = detail::registerLighting(*this); !lighting) return lighting;
        if (auto visibility = detail::registerMeshVisibility(*this); !visibility) return visibility;
        if (auto gbuffer = detail::registerGBuffer(*this); !gbuffer) return gbuffer;
        if (auto mesh_raster = detail::registerMeshRaster(*this); !mesh_raster) return mesh_raster;
        if (auto shadow = detail::registerShadow(*this); !shadow) return shadow;
        return detail::registerToneMapping(*this);
    }

    RenderResult<ResourceTypeId>
    RenderServer::registerResource(ResourceRegistration factory, ConfigValue configuration, std::span<const CommandBinding> commands) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (impl_->stop_requested_.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (auto valid = impl_->runtime_->resources_.validateFactory(factory, configuration); !valid)
            return cxx::unexpected(valid.error());
        if (auto valid = impl_->runtime_->command_registry_.validateResourceBindings(factory.storage()->implementation_type, commands); !valid)
            return cxx::unexpected(valid.error());
        auto& resources = impl_->runtime_->resources_;
        const auto type = resources.publishFactory(std::move(factory), std::move(configuration));
        for (const auto& command : commands) impl_->runtime_->command_registry_.publishResourceBinding(type, command);
        return type;
    }

    RenderResult<HandlerId>
    RenderServer::registerHandler(HandlerRecord handler, std::span<const CommandBinding> bindings) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto& state = *impl_;
        if (state.stop_requested_.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (auto valid = state.runtime_->command_registry_.validateHandler(handler, bindings); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        const auto handler_type = handler.type;
        const auto id = state.runtime_->command_registry_.publishHandler(std::move(handler));
        for (const auto& binding : bindings)
        {
            if (binding.handlerType() == handler_type) state.runtime_->command_registry_.publishBinding(id, binding);
        }
        return id;
    }

    RenderResult<void> RenderServer::registerEffectCapability(EffectNodeDefinition definition, std::vector<CommandId> commands) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        for (std::size_t i = 0; i < commands.size(); ++i)
        {
            const auto prior = std::span{commands}.first(i);
            if (std::ranges::find(prior, commands[i]) != prior.end())
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
        }
        auto node = std::make_unique<EffectNodeDefinition>(std::move(definition));
        const auto* identity = node.get();
        auto added = impl_->effect_catalog_.addCapability(std::move(node));
        if (added) impl_->runtime_->command_registry_.publishNodeCommands(*identity, std::move(commands));
        return added;
    }

    RenderResult<void> RenderServer::registerEffectImplementation(
        EffectImplementation implementation, std::vector<CommandBindingLocation> bindings
    ) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto& registry = impl_->runtime_->command_registry_;
        for (const auto binding : bindings)
        {
            const auto* command = registry.findCommand(binding.command);
            const auto* method = registry.binding(binding);
            const bool invalid_command = !command || !command->effect_node || !command->effect || command->storage;
            const bool invalid_method = !method || !method->effect_state_type.isValid() || !method->handler.isNull();
            if (invalid_command || invalid_method) return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto* node = impl_->effect_catalog_.findCapability(implementation.capability, implementation.capability_version);
        if (!node) return cxx::unexpected(RenderError{kCapabilityDeclaration});
        const auto required = registry.nodeCommands(*node);
        if (!std::ranges::is_permutation(required, bindings, std::ranges::equal_to{}, std::identity{}, &CommandBindingLocation::command))
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        auto candidate = std::make_unique<EffectImplementation>(std::move(implementation));
        const auto* identity = candidate.get();
        auto added = impl_->effect_catalog_.addImplementation(std::move(candidate));
        if (added) registry.publishImplementationBindings(*identity, std::move(bindings));
        return added;
    }

    RenderResult<CommandBindingLocation>
    RenderServer::registerNodeEffectCommand(const CommandBinding& binding) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (!binding.isNodeEffectCommand())
            return cxx::unexpected(RenderError{kCommandContract});
        auto& registry = impl_->runtime_->command_registry_;
        if (auto valid = registry.validateStaticBindings({&binding, 1}); !valid) return cxx::unexpected(valid.error());
        return registry.publishBinding({}, binding);
    }

    RenderResult<void> RenderServer::registerCommands(std::span<const CommandBinding> bindings) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (impl_->stop_requested_.load()) return cxx::unexpected(RenderError{kRuntimeState});
        if (auto valid = impl_->runtime_->command_registry_.validateStaticBindings(bindings); !valid) return valid;
        if (std::ranges::any_of(bindings, &CommandBinding::isNodeEffectCommand))
            return cxx::unexpected(RenderError{kCommandContract});
        impl_->runtime_->command_registry_.reserve(bindings.size(), 0);
        impl_->runtime_->command_registry_.publishStaticBindings(bindings);
        return {};
    }

    RenderResult<HandlerId> RenderServer::handlerId(cxx::basic_type_info type, std::string_view key) const noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto id = impl_->runtime_->command_registry_.findHandler(type, key);
        if (!id) return id;
        if (id->isNull()) return cxx::unexpected(RenderError{kRuntimeIdentity});
        return id;
    }

    RenderResult<void> RenderServer::run() noexcept
    {
        return impl_->run();
    }

    void RenderServer::stop() noexcept
    {
        impl_->stop();
    }

    RenderServerStats RenderServer::stats() const noexcept
    {
        return impl_->stats();
    }

    std::optional<RenderDevelopmentStats> RenderServer::developmentStats() const noexcept
    {
        return impl_->developmentStats();
    }
} // namespace lux::render
