#pragma once

#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>
#include <map>
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

        struct HandlerRecord final
        {
            CppType type;
            std::string name;
            std::string instance_name;
            std::unique_ptr<void, void (*)(void*) noexcept> object{nullptr, [](void*) noexcept {}};
        };

        // Only installation owns this batch. Dropping it releases every unpublished Handler.
        struct RegistrationBatch final
        {
            struct Handler final
            {
                std::unique_ptr<HandlerRecord> record;
                std::vector<CommandBinding> calls;
            };

            struct Storage final
            {
                StorageFactory factory;
                ConfigValue configuration;
                bool use_existing_configuration;
                StorageTypeId id;
            };

            struct Feature final
            {
                feature_detail::Implementation definition;
                FeatureId id;
            };

            using HandlerName = std::pair<std::string_view, std::string_view>;
            std::vector<Handler> handlers;
            std::map<HandlerName, std::size_t> handlers_by_name;
            std::vector<Storage> storage_factories;
            std::vector<Feature> features;

            void addHandler(std::unique_ptr<HandlerRecord> record, std::vector<CommandBinding> calls) noexcept
            {
                const HandlerName name{record->type.token.name(), record->instance_name};
                handlers_by_name.emplace(name, handlers.size());
                handlers.push_back({std::move(record), std::move(calls)});
            }

            void addStorage(StorageFactory factory, ConfigValue configuration, bool use_existing) noexcept
            {
                storage_factories.push_back({std::move(factory), std::move(configuration), use_existing, {}});
            }

            void addFeature(feature_detail::Implementation definition) noexcept
            {
                features.push_back({definition, {}});
            }
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
                                           factory.ops_type != cppType<typename T::Ops>();
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
            const auto found = batch.handlers_by_name.find({type.token.name(), instance});
            if (found != batch.handlers_by_name.end())
            {
                if (batch.handlers[found->second].record->type != type)
                {
                    return cxx::unexpected(RenderError{kCommandContract});
                }
                return {};
            }
            auto object = create();
            if (!object)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            auto& handler = *object;
            auto record = std::make_unique<HandlerRecord>();
            record->type = type;
            record->name = H::name;
            record->instance_name = instance;
            record->object = {object.release(), [](void* value) noexcept { delete static_cast<H*>(value); }};
            [&]<class... B>(std::tuple<B...>*) noexcept
            {
                batch.addHandler(std::move(record), {CommandBinding::bind<typename B::Command, B::method>(handler)...});
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
