#pragma once

#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <lux/engine/render/native/vulkan/Feature.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>
#include <optional>

namespace lux::render::runtime_detail
{
    struct ServerBootstrap;
    class CommandRegistry;
} // namespace lux::render::runtime_detail

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
        // Scheduler availability does not imply a persistent staging allocation.
        std::optional<TransferOptions> uploads;
        std::uint64_t gpu_wait_ns = 1'000'000; // 0..1ms; IPC/CPU/stop are checked between waits.
    };

    struct RenderServerStats final
    {
        std::uint64_t ticks = 0, commands = 0, compiled_graphs = 0, submissions = 0;
        std::uint64_t validation_errors = 0, validation_warnings = 0;
        std::uint64_t storages_created = 0;
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
        template <RenderCommand C> [[nodiscard]] RenderResult<CommandId> commandId() const noexcept
        {
            return findCommand(commandDescriptor<C>());
        }

        // Owner-thread installation; selected Native header supplies the typed definitions.
        template <class T, class Provider = StorageProvider<T>>
            requires(!std::is_void_v<typename T::Config>)
        [[nodiscard]] RenderResult<StorageTypeId> registerStorage(
            const typename T::Config& config = {},
            std::string_view handler_key = {}
        ) noexcept;

        template <class T, class Provider = StorageProvider<T>>
            requires std::is_void_v<typename T::Config>
        [[nodiscard]] RenderResult<StorageTypeId> registerStorage(std::string_view handler_key = {}) noexcept;

        template <class T, class Provider = FeatureProvider<T>, class... Args>
        [[nodiscard]] RenderResult<FeatureId> registerFeature(
            Provider provider = {},
            std::string_view handler_key = {},
            Args&&... args
        ) noexcept;

        // Cold provider installation. The caller is the Server owner; no concurrent directory mutation.
        // H::Bindings is a tuple of CommandMethod entries. Reusing type + key is idempotent.
        template <class H, class... Args>
        [[nodiscard]] RenderResult<HandlerId> emplaceHandler(std::string_view key, Args&&... args) noexcept
        {
            static_assert(std::is_nothrow_constructible_v<H, Args...> && std::is_nothrow_destructible_v<H>);
            static_assert(std::tuple_size_v<typename H::Bindings> != 0);
            const auto type = cppType<H>();
            RegistrationBatch batch;
            auto prepared = prepareHandler<H>(
                batch,
                key,
                [&]() noexcept { return std::make_unique<H>(std::forward<Args>(args)...); }
            );
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            if (auto installed = install(batch); !installed)
            {
                return cxx::unexpected(installed.error());
            }
            return findHandler(type, key);
        }

        [[nodiscard]] RenderResult<void> run() noexcept;
        void stop() noexcept;
        [[nodiscard]] RenderServerStats stats() const noexcept;

    private:
        friend struct runtime_detail::ServerBootstrap;
        friend class runtime_detail::CommandRegistry;

        struct HandlerRecord;

        // Cold transaction only. Its containers and name index are Runtime-private.
        struct RegistrationBatch final
        {
            RegistrationBatch() noexcept;
            ~RegistrationBatch() noexcept;
            RegistrationBatch(const RegistrationBatch&) = delete;
            RegistrationBatch& operator=(const RegistrationBatch&) = delete;

            [[nodiscard]] RenderResult<bool> containsHandler(CppType, std::string_view) const noexcept;
            void
                addHandler(CppType, std::string_view, std::string_view, std::unique_ptr<void, void (*)(void*) noexcept>, std::vector<CommandBinding>) noexcept;
            void addStorage(StorageFactory, ConfigValue, bool use_existing) noexcept;
            void addFeature(feature_detail::Implementation) noexcept;
            [[nodiscard]] StorageTypeId storageId() const noexcept;
            [[nodiscard]] FeatureId featureId() const noexcept;

            struct State;
            std::unique_ptr<State> state_;
        };

        template <class T, class Provider>
        RenderResult<StorageTypeId> registerStorageImpl(ConfigValue, std::string_view) noexcept;

        template <class T, class Provider>
        static RenderResult<void> prepareStorage(
            RegistrationBatch& batch,
            ConfigValue configuration,
            bool use_existing
        ) noexcept
        {
            using Config = typename T::Config;
            constexpr CppType config_type = []() noexcept
            {
                if constexpr (std::is_void_v<Config>)
                {
                    return CppType{};
                }
                else
                {
                    static_assert(requires(const Config& a, const Config& b) {
                        { a == b } noexcept -> std::same_as<bool>;
                    });
                    return cppType<Config>();
                }
            }();
            auto factory = Provider::factory();
            if (!factory.description)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            if (auto valid = compatibleStorage(T::description, *factory.description); !valid)
            {
                return valid;
            }
            const bool is_wrong_provider = factory.native_type != Provider::implementationType() ||
                                           factory.config_type != config_type ||
                                           factory.ops_type != storageOpsType<T>();
            if (is_wrong_provider)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            batch.addStorage(std::move(factory), std::move(configuration), use_existing);
            return {};
        }

        template <class H, class Make>
        RenderResult<void> prepareHandler(RegistrationBatch& batch, std::string_view instance, Make&& create) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<H>);
            static_assert(std::is_nothrow_invocable_r_v<std::unique_ptr<H>, Make>);
            const auto type = cppType<H>();
            auto installed = findHandler(type, instance);
            if (!installed)
            {
                return cxx::unexpected(installed.error());
            }
            if (!installed->isNull())
            {
                return {};
            }
            const auto present = batch.containsHandler(type, instance);
            if (!present)
            {
                return cxx::unexpected(present.error());
            }
            if (*present)
            {
                return {};
            }
            auto object = create();
            if (!object)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            auto& handler = *object;
            std::unique_ptr<void, void (*)(void*) noexcept> owner{
                object.release(),
                [](void* value) noexcept { delete static_cast<H*>(value); }
            };
            [&]<class... B>(std::tuple<B...>*) noexcept
            {
                batch.addHandler(
                    type,
                    H::name,
                    instance,
                    std::move(owner),
                    {CommandBinding::bind<typename B::Command, B::method>(handler)...}
                );
            }(static_cast<typename H::Bindings*>(nullptr));
            return {};
        }

        [[nodiscard]] RenderResult<HandlerId> findHandler(CppType, std::string_view) const noexcept;
        [[nodiscard]] RenderResult<CommandId> findCommand(const CommandDescriptor&) const noexcept;
        [[nodiscard]] RenderResult<void> install(RegistrationBatch&) noexcept;
        struct Impl;
        explicit RenderServer(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::render
