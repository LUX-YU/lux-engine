#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>

namespace lux::execution
{
    class ExecutionRuntime;
}

namespace lux::render::runtime_detail
{
    // Stateless construction access to the existing Server owner; no device or scheduler ownership here.
    struct ServerBootstrap final
    {
        static RenderResult<std::unique_ptr<RenderServer>> create(
            RenderServerConfig,
            execution::ExecutionRuntime&,
            vulkan::DeviceOptions
        ) noexcept;
        static vulkan::DeviceSession& device(RenderServer&) noexcept;
    };
} // namespace lux::render::runtime_detail

namespace lux::render::vulkan
{
    // Host runtime and all participating module code outlive Server work and callback stacks.
    [[nodiscard]] RenderResult<std::unique_ptr<RenderServer>> createRenderServer(
        RenderServerConfig config,
        execution::ExecutionRuntime& runtime,
        DeviceOptions options = {.dynamic_rendering = true}
    ) noexcept;
} // namespace lux::render::vulkan

namespace lux::render
{
    template <class T, class Provider>
        requires(!std::is_void_v<typename T::Config>)
    RenderResult<StorageTypeId> RenderServer::registerStorage(
        const typename T::Config& config,
        std::string_view handler_key
    ) noexcept
    {
        auto value = ConfigValue::make(config);
        if (!value)
        {
            return cxx::unexpected(value.error());
        }
        return registerStorageImpl<T, Provider>(std::move(*value), handler_key);
    }

    template <class T, class Provider>
        requires std::is_void_v<typename T::Config>
    RenderResult<StorageTypeId> RenderServer::registerStorage(std::string_view handler_key) noexcept
    {
        return registerStorageImpl<T, Provider>({}, handler_key);
    }

    template <class T, class Provider>
    RenderResult<StorageTypeId> RenderServer::registerStorageImpl(
        ConfigValue config,
        std::string_view handler_key
    ) noexcept
    {
        RegistrationBatch batch;
        if (auto added = prepareStorage<T, Provider>(batch, std::move(config), false); !added)
        {
            return cxx::unexpected(added.error());
        }
        using H = typename Provider::Handler;
        if constexpr (!std::is_void_v<H>)
        {
            auto prepared = prepareHandler<H>(
                batch,
                handler_key,
                [&]() noexcept { return Provider::createHandler(runtime_detail::ServerBootstrap::device(*this)); }
            );
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
        }
        if (auto installed = install(batch); !installed)
        {
            return cxx::unexpected(installed.error());
        }
        return batch.storageId();
    }

    template <class T, class Provider, class... Args>
    [[nodiscard]] RenderResult<FeatureId> RenderServer::registerFeature(
        Provider provider,
        std::string_view handler_key,
        Args&&... args
    ) noexcept
    {
        RegistrationBatch batch;
        if constexpr (requires { typename Provider::Storages; })
        {
            RenderResult<void> result;
            const auto add = [&]<class Storage>() noexcept
            {
                if (!result)
                {
                    return;
                }
                using P = StorageProvider<Storage>;
                if constexpr (std::is_void_v<typename Storage::Config>)
                {
                    result = prepareStorage<Storage, P>(batch, {}, true);
                }
                else
                {
                    auto config = ConfigValue::make(typename Storage::Config{});
                    if (!config)
                    {
                        result = cxx::unexpected(config.error());
                        return;
                    }
                    result = prepareStorage<Storage, P>(batch, std::move(*config), true);
                }
                if (!result)
                {
                    return;
                }
                using H = typename P::Handler;
                if constexpr (!std::is_void_v<H>)
                {
                    result = prepareHandler<H>(
                        batch,
                        {},
                        [&]() noexcept { return P::createHandler(runtime_detail::ServerBootstrap::device(*this)); }
                    );
                }
            };
            [&]<class... Storage>(std::tuple<Storage...>*) noexcept
            { (add.template operator()<Storage>(), ...); }(static_cast<typename Provider::Storages*>(nullptr));
            if (!result)
            {
                return cxx::unexpected(result.error());
            }
        }
        using H = typename Provider::Handler;
        if constexpr (!std::is_void_v<H>)
        {
            auto prepared = prepareHandler<H>(
                batch,
                handler_key,
                [&]() noexcept
                {
                    return provider.createHandler(
                        runtime_detail::ServerBootstrap::device(*this),
                        std::forward<Args>(args)...
                    );
                }
            );
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
        }
        batch.addFeature(provider.definition());
        if (auto installed = install(batch); !installed)
        {
            return cxx::unexpected(installed.error());
        }
        return batch.featureId();
    }
} // namespace lux::render
