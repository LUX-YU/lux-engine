#include <lux/engine/Init.hpp>
#include <lux/engine/detail/EngineContextImpl.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <system_error>
#include <utility>

#if defined(LUX_ENGINE_WITH_RENDER)
#include <lux/engine/systems/render/RenderDiagnostics.hpp>
#endif

namespace lux::engine
{
    namespace
    {
        constexpr error::ErrorDescriptor ErrorDescriptors[]{
            {
                "lux.engine.scene.configuration", "Scene configuration code {0}, offset {1}",
                error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            {"lux.engine.scene.identity_exhausted", "Scene identity space exhausted"},
            {
                "lux.engine.scene.invalid_id",
                "Scene slot {0}, generation {1} is absent",
                error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            {"lux.engine.scene.time_overflow", "Scene simulation time is exhausted"},
            {
                "lux.scene.ecs_commands", "ECS command code {0}, producer {1}, command {2}",
                error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            {
                "lux.engine.scene.graph_build",
                "Task graph code {0}, task {1}, related task {2}",
                error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            {
                "lux.engine.scene.graph_execution",
                "Task executor code {0}",
                error::ERecovery::BUG,
                {error::EArgument::UNSIGNED}
            }
        };
    } // namespace

#if defined(LUX_ENGINE_WITH_RENDER)
    InitResult init(
        execution::ExecutionRuntimeConfig configuration,
        asset::AssetDecodeLimits asset_limits,
        lux::render::RenderServerConfig rendering,
        lux::render::DeviceOptions device,
        std::unique_ptr<assets::AssetRetentionPolicy> retention,
        std::vector<std::unique_ptr<asset::AssetCodec>> additional_codecs,
        std::function<lux::render::RenderResult<void>(lux::render::RenderServer&)> register_render
    ) noexcept
#else
    InitResult init(
        execution::ExecutionRuntimeConfig configuration,
        asset::AssetDecodeLimits asset_limits,
        std::unique_ptr<assets::AssetRetentionPolicy> retention,
        std::vector<std::unique_ptr<asset::AssetCodec>> additional_codecs
    ) noexcept
#endif
    {
        auto registered = error::ErrorRegistry::instance().registerTypes(ErrorDescriptors);
        if (!registered)
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_STATE));
        }
        auto execution_errors = execution::registerExecutionErrors();
        if (!execution_errors)
        {
            return cxx::unexpected(execution_errors.error());
        }
#if defined(LUX_ENGINE_WITH_RENDER)
        auto render_errors = render::registerRenderErrors();
        if (!render_errors)
        {
            return cxx::unexpected(render_errors.error());
        }
#endif
        auto execution = execution::ExecutionRuntime::create(configuration);
        if (!execution)
        {
            return cxx::unexpected(execution::toError(execution.error()));
        }
        auto impl = std::make_unique<EngineContext::Impl>(std::move(*execution));
        auto objects = object::ObjectRuntime::create(*impl->execution);
        if (!objects)
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_STATE));
        }
        impl->objects = std::move(*objects);
        auto assets = assets::AssetRepository::create(
            impl->asset_vfs.view(), asset_limits, std::move(retention), std::move(additional_codecs)
        );
        if (!assets)
        {
            return cxx::unexpected(assets.error());
        }
        impl->assets = std::move(*assets);
#if defined(LUX_ENGINE_WITH_RENDER)
        rendering.response_context = impl.get();
        rendering.responses_available = &EngineContext::Impl::notifyResponses;
        const auto channel = rendering.channel_name;
        const auto max_frames_ahead = rendering.max_frames_ahead;
        auto server = lux::render::RenderServer::create(std::move(rendering), *impl->execution, device);
        if (!server)
        {
            return cxx::unexpected(server.error());
        }
        impl->server = std::move(*server);
        if (register_render)
        {
            auto registered_render = register_render(*impl->server);
            if (!registered_render)
            {
                return cxx::unexpected(registered_render.error());
            }
        }
        auto client = platform::ipc::IpcCore::instance().createRequester(channel);
        if (!client)
        {
            return cxx::unexpected(error::Error{lux::render::kRuntimeState});
        }
        if (auto selected = impl->server->setFrameSource(client->id()); !selected)
        {
            return cxx::unexpected(selected.error());
        }
        impl->render_responses.emplace(*impl->execution, impl.get(), [](void* state) noexcept
        {
            static_cast<EngineContext::Impl*>(state)->dispatchRenderResponses();
        });
        try
        {
            impl->render_thread = std::thread([state = impl.get()]() noexcept { state->runRender(); });
        }
        catch (const std::system_error& failure)
        {
            return cxx::unexpected(error::Error{
                lux::render::kRuntimeState, {static_cast<std::uint64_t>(failure.code().value())}
            });
        }
        auto rendering_client = lux::render::RenderClient::create(std::move(*client));
        if (!rendering_client)
        {
            // Rollback may dispatch the already queued service exit while settling the repository.
            impl->stopping_render = true;
            impl->server->stop();
            impl->render_responses->cancel(); // No published Client is available for queued wake hints.
            return cxx::unexpected(rendering_client.error());
        }
        impl->render_client = std::move(*rendering_client);
        impl->frames.emplace(*impl->render_client, max_frames_ahead);
#endif
        return std::unique_ptr<EngineContext>{new EngineContext(std::move(impl))};
    }
} // namespace lux::engine
