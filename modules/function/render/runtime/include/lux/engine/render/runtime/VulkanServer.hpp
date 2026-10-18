#pragma once

#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

namespace lux::execution { class ExecutionRuntime; }

namespace lux::render::runtime_detail
{
    // Stateless construction access to the existing Server owner; no device or scheduler ownership here.
    struct ServerBootstrap final
    {
        static RenderResult<std::unique_ptr<RenderServer>> create(
            RenderServerConfig, execution::ExecutionRuntime&, vulkan::DeviceOptions
        ) noexcept;
        static vulkan::DeviceSession& device(RenderServer&) noexcept;
    };
}

namespace lux::render::vulkan
{
    // Host runtime and all participating module code outlive Server work and callback stacks.
    [[nodiscard]] RenderResult<std::unique_ptr<RenderServer>> createRenderServer(
        RenderServerConfig config, execution::ExecutionRuntime& runtime,
        DeviceOptions options = {.dynamic_rendering = true}
    ) noexcept;
}

namespace lux::render
{
    template <class T, class Provider>
    [[nodiscard]] RenderResult<StorageTypeId>
    RenderServer::registerStorage(const typename T::Config& config, std::string_view handler_key) noexcept
    {
        auto candidate = storageCandidate<T, Provider>(config, handler_key, {});
        if (!candidate) return cxx::unexpected(candidate.error());
        return installStorage(std::move(*candidate));
    }

    template <class T, class Provider, class... Args>
    [[nodiscard]] RenderResult<FeatureId> RenderServer::registerFeature(
        Provider provider, std::string_view handler_key,
        Args&&... args
    ) noexcept
    {
        std::vector<StorageInstall> dependencies;
        if constexpr (requires { typename Provider::Storages; })
        {
            RenderResult<void> result;
            const auto add = [&]<class Storage>() noexcept {
                if (!result) return;
                auto candidate = storageCandidate<Storage, StorageProvider<Storage>>({}, {}, dependencies);
                if (!candidate) result = cxx::unexpected(candidate.error());
                else dependencies.push_back(std::move(*candidate));
            };
            [&]<class... Storage>(std::tuple<Storage...>*) noexcept {
                dependencies.reserve(sizeof...(Storage));
                (add.template operator()<Storage>(), ...);
            }(static_cast<typename Provider::Storages*>(nullptr));
            if (!result) return cxx::unexpected(result.error());
        }
        using H = typename Provider::Handler;
        const auto definition = provider.definition();
        if constexpr (std::is_void_v<H>)
        {
            return installFeature(definition, nullptr, {}, {}, std::move(dependencies));
        }
        else
        {
            const auto type = cppType<H>();
            auto existing = findHandler(type, handler_key);
            if (!existing) return cxx::unexpected(existing.error());
            std::unique_ptr<HandlerRecord> owner;
            auto* entry = existing->isNull() ? nullptr : handlerRecord(*existing);
            if (!entry)
                for (const auto& dependency : dependencies)
                {
                    auto* old = dependency.handler;
                    if (!old || old->type.token.name() != type.token.name() || old->instance_name != handler_key) continue;
                    if (old->type != type) return cxx::unexpected(RenderError{kProtocolContract});
                    entry = old;
                    break;
                }
            if (!entry)
            {
                if constexpr (sizeof...(Args) == 0 && std::is_nothrow_constructible_v<H, vulkan::DeviceSession&>)
                    owner = makeHandler<H>(handler_key, runtime_detail::ServerBootstrap::device(*this));
                else
                    owner = makeHandler<H>(handler_key, std::forward<Args>(args)...);
                entry = owner.get();
            }
            auto& handler = *static_cast<H*>(entry->object.get());
            return [&]<typename... B>(std::tuple<B...>*) noexcept {
                const std::array calls{CommandBinding::bind<typename B::Command, B::method>(handler)...};
                return installFeature(definition, entry, std::move(owner), calls, std::move(dependencies));
            }(static_cast<typename H::Bindings*>(nullptr));
        }
    }

    template <class T, class Provider>
    RenderResult<RenderServer::StorageInstall>
    RenderServer::storageCandidate(
        const typename T::Config& config, std::string_view key, std::span<const StorageInstall> previous
    ) noexcept
    {
        using H = typename Provider::Handler;
        using Config = typename T::Config;
        static_assert(requires(const Config& a, const Config& b) { { a == b } noexcept -> std::same_as<bool>; });
        auto factory = Provider::factory();
        if (!factory.description) return cxx::unexpected(RenderError{kStorageContract});
        if (auto valid = compatibleStorage(T::description, *factory.description); !valid)
            return cxx::unexpected(valid.error());
        const bool is_wrong_provider = factory.native_type != Provider::implementationType() ||
            factory.config_type != cppType<Config>() || factory.ops_type != cppType<typename T::Ops>();
        if (is_wrong_provider) return cxx::unexpected(RenderError{kStorageMismatch});
        auto configuration = ConfigValue::make(config);
        if (!configuration) return cxx::unexpected(configuration.error());
        StorageInstall result{std::move(factory), std::move(*configuration)};
        if constexpr (!std::is_void_v<H>)
        {
            const auto type = cppType<H>();
            auto existing = findHandler(type, key);
            if (!existing) return cxx::unexpected(existing.error());
            result.handler = existing->isNull() ? nullptr : handlerRecord(*existing);
            if (!result.handler)
                for (const auto& candidate : previous)
                {
                    const auto* old = candidate.handler;
                    if (!old || old->type.token.name() != type.token.name() || old->instance_name != key) continue;
                    if (old->type != type) return cxx::unexpected(RenderError{kProtocolContract});
                    result.handler = candidate.handler;
                    break;
                }
            if (!result.handler)
            {
                result.owner = makeHandler<H>(key, runtime_detail::ServerBootstrap::device(*this));
                result.handler = result.owner.get();
            }
            auto& handler = *static_cast<H*>(result.handler->object.get());
            [&]<typename... B>(std::tuple<B...>*) noexcept {
                result.calls = {CommandBinding::bind<typename B::Command, B::method>(handler)...};
            }(static_cast<typename H::Bindings*>(nullptr));
        }
        return result;
    }

}
