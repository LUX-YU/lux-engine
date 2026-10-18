#pragma once

#include <lux/engine/render/runtime/CommandBinding.hpp>
#include <lux/engine/render/runtime/Error.hpp>
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <optional>

namespace lux::render::runtime_detail { struct ServerBootstrap; class CommandRegistry; }

namespace lux::render
{
    template <class T> struct StorageProvider;
    template <class T> struct FeatureProvider;
    struct RenderServerConfig final
    {
        std::string channel_name;
        bool validation = false;
        std::uint32_t request_work_budget = 8;
        std::size_t max_config_bytes = 1024 * 1024;
        std::size_t retirement_capacity = 0; // Optional explicit record budget; zero grows on cold publication.
        // Scheduler availability does not imply a persistent staging allocation.
        std::optional<TransferOptions> uploads;
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
        ~RenderServer() noexcept;
        RenderServer(const RenderServer&) = delete;
        RenderServer& operator=(const RenderServer&) = delete;
        // Cold owner-thread query, before run() or at an owner safe point. No new identity is allocated.
        template <RenderCommand C>
        [[nodiscard]] RenderResult<CommandId> commandId() const noexcept
        {
            return findCommand(commandDescriptor<C>());
        }

        // Owner-thread installation; selected Native header supplies the typed definitions.
        template <class T, class Provider = StorageProvider<T>>
        [[nodiscard]] RenderResult<StorageTypeId>
        registerStorage(const typename T::Config& config = {}, std::string_view handler_key = {}) noexcept;

        template <class T, class Provider = FeatureProvider<T>, class... Args>
        [[nodiscard]] RenderResult<FeatureId> registerFeature(
            Provider provider = {}, std::string_view handler_key = {},
            Args&&... args
        ) noexcept;

        // Cold provider installation. The caller is the Server owner; no concurrent directory mutation.
        // H::Bindings is a tuple of CommandMethod entries. Reusing type + key is idempotent.
        template <class H, class... Args>
        [[nodiscard]] RenderResult<HandlerId> emplaceHandler(
            std::string_view key, Args&&... args
        ) noexcept
        {
            static_assert(std::is_nothrow_constructible_v<H, Args...> && std::is_nothrow_destructible_v<H>);
            static_assert(std::tuple_size_v<typename H::Bindings> != 0);
            const CommandValueType type{cxx::typeToken<H>(), sizeof(H), alignof(H)};
            auto existing = findHandler(type, key);
            if (!existing) return cxx::unexpected(existing.error());
            if (!existing->isNull()) return *existing;
            auto owner = makeHandler<H>(key, std::forward<Args>(args)...);
            auto& handler = *static_cast<H*>(owner->object.get());
            return [&]<typename... B>(std::tuple<B...>*) noexcept {
                const std::array calls{CommandBinding::bind<typename B::Command, B::method>(handler)...};
                return installHandler(std::move(owner), calls);
            }(static_cast<typename H::Bindings*>(nullptr));
        }

        [[nodiscard]] RenderResult<void> run() noexcept;
        void stop() noexcept;
        [[nodiscard]] RenderServerStats stats() const noexcept;
    private:
        friend struct runtime_detail::ServerBootstrap;
        friend class runtime_detail::CommandRegistry;

        struct HandlerRecord final
        {
            CommandValueType type;
            std::string name;
            std::string instance_name;
            std::unique_ptr<void, void (*)(void*) noexcept> object{nullptr, [](void*) noexcept {}};
        };

        // One temporary publication transaction, not a second runtime directory.
        struct StorageInstall final
        {
            StorageFactory factory;
            ConfigValue config;
            HandlerRecord* handler = nullptr;
            std::unique_ptr<HandlerRecord> owner;
            std::vector<CommandBinding> calls;
            std::vector<CommandId> commands;
        };

        template <class T, class Provider>
        RenderResult<StorageInstall> storageCandidate(const typename T::Config& config, std::string_view key,
            std::span<const StorageInstall> previous) noexcept;

        template <class H, class... Args>
        static std::unique_ptr<HandlerRecord> makeHandler(
            std::string_view key, Args&&... args
        ) noexcept
        {
            static_assert(std::is_nothrow_constructible_v<H, Args...> && std::is_nothrow_destructible_v<H>);
            auto owner = std::make_unique<HandlerRecord>();
            owner->type = {cxx::typeToken<H>(), sizeof(H), alignof(H)};
            owner->name = H::name;
            owner->instance_name = key;
            owner->object = {new H(std::forward<Args>(args)...), [](void* value) noexcept { delete static_cast<H*>(value); }};
            return owner;
        }

        [[nodiscard]] RenderResult<HandlerId> findHandler(CommandValueType, std::string_view) const noexcept;
        [[nodiscard]] RenderResult<CommandId> findCommand(const CommandDescriptor&) const noexcept;
        [[nodiscard]] HandlerRecord* handlerRecord(HandlerId) noexcept;
        [[nodiscard]] RenderResult<std::vector<CommandId>> prepareHandler(const HandlerRecord&,
            std::span<const CommandBinding>) const noexcept;
        HandlerId publishHandler(std::unique_ptr<HandlerRecord>, std::span<const CommandBinding>,
            std::vector<CommandId>) noexcept;
        [[nodiscard]] RenderResult<StorageTypeId> prepareStorage(const StorageInstall&, bool ensure) const noexcept;
        [[nodiscard]] RenderResult<StorageTypeId> installStorage(StorageInstall) noexcept;
        [[nodiscard]] RenderResult<HandlerId> installHandler(std::unique_ptr<HandlerRecord>,
            std::span<const CommandBinding>) noexcept;
        [[nodiscard]] RenderResult<FeatureId> installFeature(feature_detail::Implementation,
            HandlerRecord*, std::unique_ptr<HandlerRecord>,
            std::span<const CommandBinding>,
            std::vector<StorageInstall> dependencies = {}) noexcept;
        struct Impl;
        explicit RenderServer(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
