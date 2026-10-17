#pragma once

#include <lux/engine/render/runtime/CommandValue.hpp>
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/resource/AsyncWork.hpp>
#include <lux/engine/render/resource/Registration.hpp>
#include <optional>
#include <lux/engine/render/native/vulkan/device/DeviceOptions.hpp>

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
        std::size_t retirement_capacity = 64;
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
        template <RenderCommand... C>
        [[nodiscard]] RenderResult<FeatureId> registerFeature(FeatureRegistration feature) noexcept
        {
            const std::array<runtime_detail::CommandTransport, sizeof...(C)> transports{runtime_detail::commandTransport<C>()...};
            return registerFeatureImpl(std::move(feature), transports);
        }

        template <RenderCommand... C>
        [[nodiscard]] RenderResult<void> registerStorage(StorageRegistration storage) noexcept
        {
            const std::array<runtime_detail::CommandTransport, sizeof...(C)> transports{runtime_detail::commandTransport<C>()...};
            return registerStorageImpl(std::move(storage), transports);
        }

        template <class T>
        [[nodiscard]] RenderResult<void> registerStorage(const typename T::Config& config = {}) noexcept
        {
            auto registration = nativeStorageBinding<T>();
            if (!registration.description) return cxx::unexpected(RenderError{kStorageContract});
            if (auto valid = compatibleStorage(T::description, *registration.description); !valid) return valid;
            auto configuration = ConfigValue::make(config, registration.factory.code_pin);
            if (!configuration) return cxx::unexpected(configuration.error());
            registration.configuration = std::move(*configuration);
            return [&]<typename... C>(std::tuple<C...>*) noexcept {
                return registerStorage<C...>(std::move(registration));
            }(static_cast<typename T::Commands*>(nullptr));
        }

        template <class T>
        [[nodiscard]] RenderResult<FeatureId> registerFeature() noexcept
        {
            return [&]<typename... C>(std::tuple<C...>*) noexcept {
                return registerFeature<C...>(nativeFeatureBinding<T>());
            }(static_cast<typename T::Commands*>(nullptr));
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
            auto owner = std::make_unique<HandlerEntry>();
            owner->code_pin = std::move(code_pin);
            owner->type = type;
            owner->name = H::name;
            owner->key = key;
            auto object = std::make_unique<H>(std::forward<Args>(args)...);
            auto& handler = *object;
            owner->object = {object.release(), [](void* value) noexcept { delete static_cast<H*>(value); }};
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

        [[nodiscard]] RenderResult<HandlerId> findHandler(CommandValueType, std::string_view) const noexcept;
        [[nodiscard]] RenderResult<HandlerId> installHandler(std::unique_ptr<HandlerEntry>,
            std::span<const CommandHandler>, std::span<const runtime_detail::CommandTransport>) noexcept;
        [[nodiscard]] RenderResult<FeatureId> registerFeatureImpl(FeatureRegistration,
            std::span<const runtime_detail::CommandTransport>) noexcept;
        [[nodiscard]] RenderResult<void> registerStorageImpl(StorageRegistration,
            std::span<const runtime_detail::CommandTransport>) noexcept;
        struct Impl;
        explicit RenderServer(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
