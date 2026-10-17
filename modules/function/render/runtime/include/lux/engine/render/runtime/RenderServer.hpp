#pragma once

#include <lux/engine/render/protocol/CommandHandler.hpp>
#include <lux/engine/render/runtime/Error.hpp>
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/resource/AsyncWork.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <optional>
#include <lux/engine/render/native/vulkan/device/DeviceOptions.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureCommands.hpp>

namespace lux::render::vulkan { template <class T> struct FeatureProvider; }

namespace lux::render
{
    struct RenderServerConfig final
    {
        std::string channel_name;
        bool validation = false;
        vulkan::DeviceOptions device{.dynamic_rendering = true};
        std::uint32_t requests_per_tick = 8;
        std::size_t max_config_bytes = 1024 * 1024;
        std::size_t retirement_capacity = 0; // Optional explicit record budget; zero grows on cold publication.
        // One borrowed host scheduler for cold compilation and transfer preparation.
        CpuSchedule cpu;
        // Scheduler availability does not imply a persistent staging allocation.
        std::optional<TransferOptions> uploads;
        // Pins the module containing this Server implementation and its builtin reply/destruction code.
        // Empty explicitly declares process-lifetime code. The host retains its own pin through factory,
        // run/join and destruction calls; published replies retain theirs independently of the Server.
        std::shared_ptr<const void> implementation_code_pin;
    };

    struct RenderServerStats final
    {
        std::uint64_t ticks = 0, commands = 0, compiled_graphs = 0, submissions = 0;
        std::uint64_t validation_errors = 0, validation_warnings = 0;
        std::uint64_t scoped_buffers_created = 0;
    };

    // The host owns the rendering thread. Create initializes Native before cold installation;
    // run transfers exclusive mutation to its calling thread. Only stop()/stats() may race with run().
    class RenderServer final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderServer>> create(RenderServerConfig config) noexcept;
        ~RenderServer() noexcept;
        RenderServer(const RenderServer&) = delete;
        RenderServer& operator=(const RenderServer&) = delete;
        // Cold owner-thread query, before run() or at an owner safe point. No new identity is allocated.
        template <RenderCommand C>
        [[nodiscard]] RenderResult<CommandId> commandId() const noexcept
        {
            return findCommand(commandDescriptor<C>());
        }

        // Provider pins its Native factory code. source_code_pin additionally covers the calling
        // module's template thunks/configuration; omitted means the factory module or process lifetime.
        template <class T, class Provider = vulkan::StorageProvider<T>>
        [[nodiscard]] RenderResult<StorageTypeId>
        registerStorage(const typename T::Config& config = {}, std::string_view handler_key = {},
            std::shared_ptr<const void> source_code_pin = {}) noexcept
        {
            auto candidate = storageCandidate<T, Provider>(config, handler_key, std::move(source_code_pin), {});
            if (!candidate) return cxx::unexpected(candidate.error());
            return installStorage(std::move(*candidate));
        }

        template <class T, class Provider = vulkan::FeatureProvider<T>, class... Args>
        [[nodiscard]] RenderResult<FeatureId> registerFeature(
            Provider provider = {}, std::string_view handler_key = {}, std::shared_ptr<const void> code_pin = {},
            Args&&... args
        ) noexcept
        {
            auto dependencies = featureStorages<Provider>(code_pin);
            if (!dependencies) return cxx::unexpected(dependencies.error());
            using H = typename Provider::Handler;
            const auto definition = provider.definition();
            if constexpr (std::is_void_v<H>)
            {
                return installFeature(definition, std::move(code_pin), nullptr, {}, {}, std::move(*dependencies));
            }
            else
            {
                const CommandValueType type{cxx::typeToken<H>(), sizeof(H), alignof(H)};
                auto existing = findHandler(type, handler_key);
                if (!existing) return cxx::unexpected(existing.error());
                std::unique_ptr<HandlerEntry> owner;
                auto* entry = existing->isNull() ? nullptr : handlerEntry(*existing);
                if (!entry)
                    for (const auto& dependency : *dependencies)
                    {
                        auto* old = dependency.handler;
                        if (!old || old->type.type.name() != type.type.name() || old->key != handler_key) continue;
                        if (old->type != type) return cxx::unexpected(RenderError{kProtocolContract});
                        entry = old;
                        break;
                    }
                if (!entry)
                {
                    if constexpr (sizeof...(Args) == 0 && std::is_nothrow_constructible_v<H, vulkan::DeviceSession&>)
                        owner = makeHandler<H>(handler_key, code_pin, nativeDevice());
                    else
                        owner = makeHandler<H>(handler_key, code_pin, std::forward<Args>(args)...);
                    entry = owner.get();
                }
                auto& handler = *static_cast<H*>(entry->object.get());
                return [&]<typename... B>(std::tuple<B...>*) noexcept {
                    const std::array calls{B::bind(handler)...};
                    return installFeature(definition, std::move(code_pin), entry, std::move(owner),
                        calls, std::move(*dependencies));
                }(static_cast<typename H::Bindings*>(nullptr));
            }
        }

        // Cold provider installation. The caller is the Server owner; no concurrent directory mutation.
        // H::Bindings is a tuple of CommandMethod entries. Reusing type + key is idempotent.
        template <class H, class... Args>
        [[nodiscard]] RenderResult<HandlerId> emplaceHandler(
            std::string_view key, std::shared_ptr<const void> code_pin, Args&&... args
        ) noexcept
        {
            static_assert(std::is_nothrow_constructible_v<H, Args...> && std::is_nothrow_destructible_v<H>);
            static_assert(std::tuple_size_v<typename H::Bindings> != 0);
            const CommandValueType type{cxx::typeToken<H>(), sizeof(H), alignof(H)};
            auto existing = findHandler(type, key);
            if (!existing) return cxx::unexpected(existing.error());
            if (!existing->isNull()) return *existing;
            auto owner = makeHandler<H>(key, std::move(code_pin), std::forward<Args>(args)...);
            auto& handler = *static_cast<H*>(owner->object.get());
            return [&]<typename... B>(std::tuple<B...>*) noexcept {
                const std::array calls{B::bind(handler)...};
                return installHandler(std::move(owner), calls);
            }(static_cast<typename H::Bindings*>(nullptr));
        }

        [[nodiscard]] RenderResult<void> run() noexcept;
        void stop() noexcept;
        [[nodiscard]] RenderServerStats stats() const noexcept;
    private:
        struct HandlerEntry final
        {
            // Foreign object/deleter destruction precedes release of its source module.
            std::shared_ptr<const void> code_pin;
            HandlerId id;
            CommandValueType type;
            std::string name;
            std::string key;
            std::unique_ptr<void, void (*)(void*) noexcept> object{nullptr, [](void*) noexcept {}};
        };

        // One temporary publication transaction, not a second runtime directory.
        struct StorageInstall final
        {
            StorageFactory factory;
            ConfigValue config;
            bool (*equal)(const void*, const void*) noexcept = nullptr;
            HandlerEntry* handler = nullptr;
            std::unique_ptr<HandlerEntry> owner;
            std::vector<CommandHandler> calls;
            std::vector<CommandId> commands;
        };

        template <class T, class Provider>
        RenderResult<StorageInstall> storageCandidate(const typename T::Config& config, std::string_view key,
            std::shared_ptr<const void> pin, std::span<const StorageInstall> previous) noexcept
        {
            using H = typename Provider::Handler;
            using Config = typename T::Config;
            static_assert(requires(const Config& a, const Config& b) { { a == b } noexcept -> std::same_as<bool>; });
            auto factory = Provider::factory();
            if (!factory.description) return cxx::unexpected(RenderError{kStorageContract});
            if (auto valid = compatibleStorage(T::description, *factory.description); !valid)
                return cxx::unexpected(valid.error());
            const bool is_wrong_provider = factory.native_type != Provider::implementationType() ||
                factory.config_type != storageCppType<Config>() || factory.ops_type != storageCppType<typename T::Ops>();
            if (is_wrong_provider) return cxx::unexpected(RenderError{kStorageMismatch});
            if (!pin) pin = factory.code_pin;
            auto configuration = ConfigValue::make(config, pin);
            if (!configuration) return cxx::unexpected(configuration.error());
            StorageInstall result{std::move(factory), std::move(*configuration)};
            result.equal = +[](const void* a, const void* b) noexcept {
                return *static_cast<const Config*>(a) == *static_cast<const Config*>(b);
            };
            if constexpr (!std::is_void_v<H>)
            {
                const CommandValueType type{cxx::typeToken<H>(), sizeof(H), alignof(H)};
                auto existing = findHandler(type, key);
                if (!existing) return cxx::unexpected(existing.error());
                result.handler = existing->isNull() ? nullptr : handlerEntry(*existing);
                if (!result.handler)
                    for (const auto& candidate : previous)
                    {
                        const auto* old = candidate.handler;
                        if (!old || old->type.type.name() != type.type.name() || old->key != key) continue;
                        if (old->type != type) return cxx::unexpected(RenderError{kProtocolContract});
                        result.handler = candidate.handler;
                        break;
                    }
                if (!result.handler)
                {
                    result.owner = makeHandler<H>(key, std::move(pin), nativeDevice());
                    result.handler = result.owner.get();
                }
                auto& handler = *static_cast<H*>(result.handler->object.get());
                [&]<typename... B>(std::tuple<B...>*) noexcept {
                    result.calls = {B::bind(handler)...};
                }(static_cast<typename H::Bindings*>(nullptr));
            }
            return result;
        }

        template <class Provider>
        RenderResult<std::vector<StorageInstall>> featureStorages(const std::shared_ptr<const void>& pin) noexcept
        {
            std::vector<StorageInstall> candidates;
            if constexpr (requires { typename Provider::Storages; })
            {
                RenderResult<void> result;
                const auto add = [&]<class T>() noexcept {
                    if (!result) return;
                    auto candidate = storageCandidate<T, vulkan::StorageProvider<T>>({}, {}, pin, candidates);
                    if (!candidate) result = cxx::unexpected(candidate.error());
                    else candidates.push_back(std::move(*candidate));
                };
                [&]<class... T>(std::tuple<T...>*) noexcept {
                    candidates.reserve(sizeof...(T));
                    (add.template operator()<T>(), ...);
                }(static_cast<typename Provider::Storages*>(nullptr));
                if (!result) return cxx::unexpected(result.error());
            }
            return candidates;
        }

        template <class H, class... Args>
        static std::unique_ptr<HandlerEntry> makeHandler(
            std::string_view key, std::shared_ptr<const void> pin, Args&&... args
        ) noexcept
        {
            static_assert(std::is_nothrow_constructible_v<H, Args...> && std::is_nothrow_destructible_v<H>);
            auto owner = std::make_unique<HandlerEntry>();
            owner->code_pin = std::move(pin);
            owner->type = {cxx::typeToken<H>(), sizeof(H), alignof(H)};
            owner->name = H::name;
            owner->key = key;
            owner->object = {new H(std::forward<Args>(args)...), [](void* value) noexcept { delete static_cast<H*>(value); }};
            return owner;
        }

        [[nodiscard]] RenderResult<HandlerId> findHandler(CommandValueType, std::string_view) const noexcept;
        [[nodiscard]] RenderResult<CommandId> findCommand(const CommandDescriptor&) const noexcept;
        [[nodiscard]] HandlerEntry* handlerEntry(HandlerId) noexcept;
        [[nodiscard]] vulkan::DeviceSession& nativeDevice() noexcept;
        [[nodiscard]] RenderResult<std::vector<CommandId>> prepareHandler(const HandlerEntry&,
            std::span<const CommandHandler>) const noexcept;
        HandlerId publishHandler(std::unique_ptr<HandlerEntry>, std::span<const CommandHandler>,
            std::vector<CommandId>) noexcept;
        [[nodiscard]] RenderResult<StorageTypeId> prepareStorage(const StorageInstall&, bool ensure) const noexcept;
        [[nodiscard]] RenderResult<StorageTypeId> installStorage(StorageInstall) noexcept;
        [[nodiscard]] RenderResult<HandlerId> installHandler(std::unique_ptr<HandlerEntry>,
            std::span<const CommandHandler>) noexcept;
        [[nodiscard]] RenderResult<FeatureId> installFeature(feature_detail::Implementation,
            std::shared_ptr<const void>, HandlerEntry*, std::unique_ptr<HandlerEntry>,
            std::span<const CommandHandler>,
            std::vector<StorageInstall> dependencies = {}) noexcept;
        struct Impl;
        explicit RenderServer(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
