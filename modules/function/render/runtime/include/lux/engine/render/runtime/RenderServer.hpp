#pragma once

#include <lux/engine/render/runtime/CommandValue.hpp>
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
        std::size_t max_reply_bytes = 16 * 1024 * 1024;
        std::size_t retirement_capacity = 0; // Optional explicit record budget; zero grows on cold publication.
        // One borrowed host scheduler for cold compilation and transfer preparation.
        CpuSchedule cpu;
        // Independent logical budgets backed by one slot map and event queue of their combined capacity.
        std::size_t compile_capacity = 2;
        std::size_t completion_capacity = 128;
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
        // Provider pins its Native factory code. source_code_pin additionally covers the calling
        // module's template thunks/configuration; omitted means the factory module or process lifetime.
        template <class T, class Provider = vulkan::StorageProvider<T>>
        [[nodiscard]] RenderResult<StorageTypeId>
        registerStorage(const typename T::Config& config = {}, std::string_view handler_key = {},
            std::shared_ptr<const void> source_code_pin = {}) noexcept
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
            if (!source_code_pin) source_code_pin = factory.code_pin;
            auto configuration = ConfigValue::make(config, source_code_pin);
            if (!configuration) return cxx::unexpected(configuration.error());
            static constexpr auto equal = +[](const void* a, const void* b) noexcept {
                return *static_cast<const Config*>(a) == *static_cast<const Config*>(b);
            };
            if constexpr (std::is_void_v<H>)
            {
                return installStorage(std::move(factory), std::move(*configuration), equal, nullptr, {}, {}, {});
            }
            else
            {
                const CommandValueType type{cxx::typeToken<H>(), sizeof(H), alignof(H)};
                auto existing = findHandler(type, handler_key);
                if (!existing) return cxx::unexpected(existing.error());
                std::unique_ptr<HandlerEntry> owner;
                auto* entry = existing->isNull() ? nullptr : handlerEntry(*existing);
                if (!entry)
                {
                    owner = makeHandler<H>(handler_key, std::move(source_code_pin), nativeDevice());
                    entry = owner.get();
                }
                auto& handler = *static_cast<H*>(entry->object.get());
                return [&]<typename... B>(std::tuple<B...>*) noexcept {
                    const std::array calls{B::bind(handler)...};
                    const std::array transports{runtime_detail::commandTransport<typename B::Command>()...};
                    return installStorage(std::move(factory), std::move(*configuration), equal, entry,
                        std::move(owner), calls, transports);
                }(static_cast<typename H::Bindings*>(nullptr));
            }
        }

        template <class T, class Provider = vulkan::FeatureProvider<T>, class... Args>
        [[nodiscard]] RenderResult<FeatureId> registerFeature(
            Provider provider = {}, std::string_view handler_key = {}, std::shared_ptr<const void> code_pin = {},
            Args&&... args
        ) noexcept
        {
            using H = typename Provider::Handler;
            const auto definition = provider.definition();
            if constexpr (std::is_void_v<H>)
            {
                return installFeature(definition, std::move(code_pin), nullptr, {}, {}, {});
            }
            else
            {
                const CommandValueType type{cxx::typeToken<H>(), sizeof(H), alignof(H)};
                auto existing = findHandler(type, handler_key);
                if (!existing) return cxx::unexpected(existing.error());
                std::unique_ptr<HandlerEntry> owner;
                auto* entry = existing->isNull() ? nullptr : handlerEntry(*existing);
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
                    const std::array transports{runtime_detail::commandTransport<typename B::Command>()...};
                    return installFeature(definition, std::move(code_pin), entry, std::move(owner), calls, transports);
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
                const std::array transports{runtime_detail::commandTransport<typename B::Command>()...};
                return installHandler(std::move(owner), calls, transports);
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
        [[nodiscard]] HandlerEntry* handlerEntry(HandlerId) noexcept;
        [[nodiscard]] vulkan::DeviceSession& nativeDevice() noexcept;
        [[nodiscard]] RenderResult<std::vector<CommandId>> prepareHandler(const HandlerEntry&,
            std::span<const CommandHandler>, std::span<const runtime_detail::CommandTransport>) const noexcept;
        HandlerId publishHandler(std::unique_ptr<HandlerEntry>, std::span<const CommandHandler>,
            std::span<const runtime_detail::CommandTransport>, std::vector<CommandId>) noexcept;
        [[nodiscard]] RenderResult<StorageTypeId> installStorage(StorageFactory, ConfigValue,
            bool (*)(const void*, const void*) noexcept, HandlerEntry*, std::unique_ptr<HandlerEntry>,
            std::span<const CommandHandler>, std::span<const runtime_detail::CommandTransport>) noexcept;
        [[nodiscard]] RenderResult<HandlerId> installHandler(std::unique_ptr<HandlerEntry>,
            std::span<const CommandHandler>, std::span<const runtime_detail::CommandTransport>) noexcept;
        [[nodiscard]] RenderResult<FeatureId> installFeature(feature_detail::Implementation,
            std::shared_ptr<const void>, HandlerEntry*, std::unique_ptr<HandlerEntry>,
            std::span<const CommandHandler>, std::span<const runtime_detail::CommandTransport>) noexcept;
        struct Impl;
        explicit RenderServer(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
