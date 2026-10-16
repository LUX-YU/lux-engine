#pragma once

#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/features/Assembly.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/resource/CpuSchedule.hpp>
#include <lux/engine/render/resource/Registration.hpp>
#include <optional>

namespace lux::render
{
    struct RenderServerConfig final
    {
        std::string channel_name;
        bool validation = false;
        std::uint32_t physical_device_index = UINT32_MAX;
        std::uint32_t requests_per_tick = 8;
        std::size_t max_config_bytes = 1024 * 1024;
        std::size_t max_reply_bytes = 16 * 1024 * 1024;
        std::size_t retirement_capacity = 64;
        // One borrowed host scheduler for cold compilation and transfer preparation.
        CpuSchedule cpu;
        // Independent logical budgets backed by one slot map and event queue of their combined capacity.
        std::size_t compile_capacity = 2;
        std::size_t completion_capacity = 128;
        // Cold explicit Native capability request. Aliased roles still share one physical queue owner.
        bool transfer_queues = false;
        bool prefer_dedicated_transfer = true;
        // Scheduler availability does not imply a persistent staging allocation.
        std::optional<TransferOptions> uploads;
        bool texture_compression_bc = false; // explicitly request BC capability; RGBA-only hosts do not require it
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

    // The host owns the rendering thread. Create/register are cold; run owns all
    // object and Native mutation. Only stop()/stats() may race with run().
    class RenderServer final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderServer>> create(RenderServerConfig config) noexcept;
        ~RenderServer() noexcept;
        RenderServer(const RenderServer&) = delete;
        RenderServer& operator=(const RenderServer&) = delete;
        template <RenderCommand... C>
        [[nodiscard]] RenderResult<void> registerFeature(FeatureRegistration feature) noexcept
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
            registration.configuration_type = storageCppType<typename T::Config>();
            registration.configuration = std::make_shared<const typename T::Config>(config);
            return [&]<typename... C>(std::tuple<C...>*) noexcept {
                return registerStorage<C...>(std::move(registration));
            }(static_cast<typename T::Commands*>(nullptr));
        }

        template <class T>
        [[nodiscard]] RenderResult<void> registerFeature() noexcept
        {
            return [&]<typename... C>(std::tuple<C...>*) noexcept {
                return registerFeature<C...>(nativeFeatureBinding<T>());
            }(static_cast<typename T::Commands*>(nullptr));
        }

        [[nodiscard]] RenderResult<void> run() noexcept;
        void stop() noexcept;
        [[nodiscard]] RenderServerStats stats() const noexcept;
    private:
        [[nodiscard]] RenderResult<void> registerFeatureImpl(FeatureRegistration,
            std::span<const runtime_detail::CommandTransport>) noexcept;
        [[nodiscard]] RenderResult<void> registerStorageImpl(StorageRegistration,
            std::span<const runtime_detail::CommandTransport>) noexcept;
        struct Impl;
        explicit RenderServer(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
