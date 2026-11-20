#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/effect/BuiltinEffectNodes.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/resource/instance/RasterMeshInstanceStorage.hpp>
#include <lux/engine/render/resource/lighting/DirectLightingStorage.hpp>
#include <lux/engine/render/resource/material/RasterMaterialStorage.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/texture/SampledTexture2DStorage.hpp>
#include <lux/engine/render/resource/transform/TransformStorage.hpp>
#include <lux/engine/render/resource/view/ViewParameterStorage.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
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

        auto state = std::make_unique<RenderServer::Impl>(std::move(config), std::move(*responder), execution);
        auto server = std::unique_ptr<RenderServer>(new RenderServer(std::move(state)));
        if (auto registered = server->registerBuiltins(); !registered)
        {
            return cxx::unexpected(registered.error());
        }

        return server;
    }

    RenderResult<void> RenderServer::registerBuiltins() noexcept
    {
        using Storages = std::tuple<
            detail::SampledTexture2DStorage, detail::IndexedTriangleMeshStorage, detail::RasterMaterialStorage,
            detail::RasterMeshInstanceStorage, detail::TransformStorage, detail::DirectLightingStorage
        >;
        if (auto added = registerResource(detail::ViewParameterStorage::factory()); !added)
            return cxx::unexpected(added.error());
        RenderResult<void> registered;
        cxx::tuple_traits::for_each_type<Storages>([&]<class Storage, std::size_t>() noexcept
        {
            if (!registered) return;
            auto added = registerResource<Storage>();
            if (!added) registered = cxx::unexpected(added.error());
        });
        if (!registered) return registered;
        if (auto depth = detail::registerDepthPyramid(*this); !depth) return depth;
        if (auto lighting = detail::registerLighting(*this); !lighting) return lighting;
        if (auto visibility = detail::registerMeshVisibility(*this); !visibility) return visibility;
        if (auto gbuffer = detail::registerGBuffer(*this); !gbuffer) return gbuffer;
        if (auto mesh_raster = detail::registerMeshRaster(*this); !mesh_raster) return mesh_raster;
        if (auto shadow = detail::registerShadow(*this); !shadow) return shadow;
        return detail::registerToneMapping(*this);
    }

    RenderResult<ResourceTypeId>
    RenderServer::registerResource(ResourceRegistration factory, ConfigValue configuration) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (impl_->stop_requested_.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (auto valid = impl_->runtime_->resources_.validateFactory(factory, configuration); !valid)
            return cxx::unexpected(valid.error());
        if (auto valid = impl_->runtime_->command_registry_.validateResourceBindings(factory.commands); !valid)
            return cxx::unexpected(valid.error());
        const auto type = impl_->runtime_->resources_.publishFactory(factory, std::move(configuration));
        for (const auto& command : factory.commands)
        {
            const auto location = impl_->runtime_->command_registry_.publishResourceBinding(command);
            if (!impl_->runtime_->resources_.bindCommand(type, location)) std::terminate();
        }
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

    RenderResult<void> RenderServer::registerEffectCapability(CapabilityDefinition definition) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        return impl_->effect_catalog_.addCapability(std::make_unique<CapabilityDefinition>(std::move(definition)));
    }

    RenderResult<void> RenderServer::registerEffectImplementation(EffectImplementation implementation) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        const auto& registry = impl_->runtime_->command_registry_;
        for (const auto binding : implementation.command_bindings)
        {
            const auto* command = registry.findCommand(binding.command);
            const auto* method = registry.binding(binding);
            const bool invalid_command = !command || !command->effect_node || !command->effect || command->storage;
            const bool invalid_method = !method || !method->effect_state_type.isValid() || !method->handler.isNull();
            if (invalid_command || invalid_method) return cxx::unexpected(RenderError{kCommandContract});
        }
        return impl_->effect_catalog_.addImplementation(std::make_unique<EffectImplementation>(std::move(implementation)));
    }

    RenderResult<CommandBindingLocation>
    RenderServer::registerNodeEffectCommand(const CommandBinding& binding, object::CodeLease code) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (!code.valid() || !binding.isNodeEffectCommand())
            return cxx::unexpected(RenderError{kCommandContract});
        auto& runtime = *impl_->runtime_;
        auto& registry = runtime.command_registry_;
        if (auto valid = registry.validateStaticBindings({&binding, 1}); !valid) return cxx::unexpected(valid.error());
        runtime.effect_command_code_.push_back(std::move(code));
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
