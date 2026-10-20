#pragma once

#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>
#include <lux/engine/render/visibility.h>
#include <optional>

namespace lux::execution
{
    class ExecutionRuntime;
}

namespace lux::render::detail
{
    class DeviceSession;
}

namespace lux::render::runtime_detail
{
    class CommandRegistry;
} // namespace lux::render::runtime_detail

namespace lux::render
{
    struct StorageFactory;
    struct FeatureDefinition;

    struct RenderServerConfig final
    {
        std::string channel_name;
        bool validation = false;
        std::uint32_t request_work_budget = 8;
        std::size_t max_config_bytes = 1024 * 1024;
        // Scheduler availability does not imply a persistent staging allocation.
        std::optional<TransferOptions> uploads;
        std::uint32_t frames_in_flight = 2;
        std::uint64_t gpu_wait_ns = 1'000'000; // 0..1ms; IPC/CPU/stop are checked between waits.
    };

    struct RenderServerStats final
    {
        std::uint64_t ticks = 0, commands = 0, compiled_graphs = 0, submissions = 0;
        std::uint64_t validation_errors = 0, validation_warnings = 0;
        std::uint64_t storages_created = 0;
    };

    class LUX_RENDER_PUBLIC RenderServer;
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<std::unique_ptr<RenderServer>> createRenderServer(
        RenderServerConfig config,
        execution::ExecutionRuntime& runtime,
        DeviceOptions options = {}
    ) noexcept;

    // The application starts and joins the rendering thread. Create initializes the device before installation;
    // run transfers exclusive mutation to its calling thread. Only stop()/stats() may race with run().
    class LUX_RENDER_PUBLIC RenderServer final
    {
    public:
        ~RenderServer() noexcept;
        RenderServer(const RenderServer&) = delete;
        RenderServer& operator=(const RenderServer&) = delete;

        // Cold query before run() or at a RenderThread safe point. No new identity is allocated.
        template <RenderCommand C> [[nodiscard]] RenderResult<CommandId> commandId() const noexcept
        {
            return findCommand(commandDescriptor<C>());
        }

        // Description-only registration; implementation definitions are compiled in the defining module.
        template <class Descriptor>
            requires(!std::is_void_v<typename Descriptor::Config>)
        [[nodiscard]] RenderResult<StorageTypeId> registerStorage(
            const typename Descriptor::Config& config = {},
            std::string_view handler_key = {}
        ) noexcept;

        template <class Descriptor>
            requires std::is_void_v<typename Descriptor::Config>
        [[nodiscard]] RenderResult<StorageTypeId> registerStorage(std::string_view handler_key = {}) noexcept;

        template <class Descriptor>
        [[nodiscard]] RenderResult<FeatureId> registerFeature(std::string_view handler_key = {}) noexcept;

        // Cold Handler installation before run() or on RenderThread; no concurrent directory mutation.
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
        friend LUX_RENDER_PUBLIC RenderResult<std::unique_ptr<RenderServer>> createRenderServer(
            RenderServerConfig,
            execution::ExecutionRuntime&,
            DeviceOptions
        ) noexcept;
        friend class runtime_detail::CommandRegistry;

        struct HandlerRecord;

        // Cold transaction only. Its containers and name index are Runtime-private.
        struct LUX_RENDER_PUBLIC RegistrationBatch final
        {
            using HandlerObject = std::unique_ptr<void, void (*)(void*) noexcept>;

            RegistrationBatch() noexcept;
            ~RegistrationBatch() noexcept;
            RegistrationBatch(const RegistrationBatch&) = delete;
            RegistrationBatch& operator=(const RegistrationBatch&) = delete;

            [[nodiscard]] RenderResult<bool> containsHandler(CppType, std::string_view) const noexcept;
            void
                addHandler(CppType, std::string_view, std::string_view, HandlerObject, std::vector<CommandBinding>) noexcept;
            void addStorage(StorageFactory, ConfigValue, bool use_existing) noexcept;
            void addFeature(FeatureDefinition) noexcept;
            [[nodiscard]] StorageTypeId storageId() const noexcept;
            [[nodiscard]] FeatureId featureId() const noexcept;

            struct State;
            std::unique_ptr<State> state_;
        };

        // The same registration transaction is used by built-ins and project extensions.
        template <class Descriptor>
        RenderResult<void> prepareStorage(
            RegistrationBatch&,
            ConfigValue,
            bool use_existing,
            std::string_view handler_key
        ) noexcept;

        template <class Descriptor>
        RenderResult<void> prepareFeature(RegistrationBatch&, std::string_view handler_key) noexcept;

        template <class Descriptor, class Storage, class Handler>
        RenderResult<void> prepareStorageDefinition(
            RegistrationBatch&,
            StorageFactory,
            ConfigValue,
            bool use_existing,
            std::string_view handler_key
        ) noexcept;

        [[nodiscard]] detail::DeviceSession& device() noexcept;

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
            auto& handler = *object;
            std::unique_ptr<void, void (*)(void*) noexcept> handler_object{
                object.release(),
                [](void* value) noexcept { delete static_cast<H*>(value); }
            };
            [&]<class... B>(std::tuple<B...>*) noexcept
            {
                batch.addHandler(
                    type,
                    H::name,
                    instance,
                    std::move(handler_object),
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
