#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/render/features/FeatureDescriptor.hpp>
#include <lux/engine/render/resource/StorageDescriptor.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>
#include <lux/engine/render/visibility.h>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace lux::execution
{
    class ExecutionRuntime;
}

namespace lux::render
{
    struct StorageFactory;
    struct FeatureGraphOps;
    class CommandBinding;

    namespace runtime_detail
    {
        struct HandlerRecord;
    }

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
        RenderErrorSink diagnostics;
    };

    struct RenderServerStats final
    {
        std::uint64_t ticks = 0, commands = 0, compiled_graphs = 0, submissions = 0;
        std::uint64_t validation_errors = 0, validation_warnings = 0;
        std::uint64_t storages_created = 0;
    };

    // The application owns the rendering thread. Only stop() and stats() may race with run().
    // Stop and join that thread before destruction; the borrowed ExecutionRuntime must outlive this Server.
    class LUX_RENDER_PUBLIC RenderServer final
    {
    public:
        using CreateResult = RenderResult<std::unique_ptr<RenderServer>>;

        // Creates the device, IPC responder and supported built-in declarations, but starts no thread.
        // Registration stores factories only. run() constructs selected Handlers; PrepareStorage/Attach create Storage.
        [[nodiscard]] static CreateResult
        create(
            RenderServerConfig config,
            execution::ExecutionRuntime& execution,
            DeviceOptions device_options = {}
        ) noexcept;

        ~RenderServer() noexcept;
        RenderServer(const RenderServer&) = delete;
        RenderServer& operator=(const RenderServer&) = delete;

        // Cold lookup before run() or at a RenderThread safe point; never allocates a CommandId.
        template <RenderCommand C> [[nodiscard]] RenderResult<CommandId> commandId() const noexcept
        {
            return findCommand(commandDescriptor<C>());
        }

        // Cold built-in discovery; does not register a dependency or construct an implementation.
        template <class Descriptor> [[nodiscard]] RenderResult<FeatureId> featureId() const noexcept
        {
            return findFeature(Descriptor::descriptor);
        }

        // Project extensions include runtime/FeatureRegistration.hpp for these template definitions.
        // Metadata and callback code must outlive the Server and all accepted work. Register before
        // run() or at a RenderThread safe point. Failed validation leaves all directories unchanged.
        template <class Feature> [[nodiscard]] RenderResult<FeatureId> registerFeature() noexcept;

        // Only installs the Storage factory/default. Commands are declared by the enclosing Feature.
        template <class Storage> requires(!std::is_void_v<typename Storage::Descriptor::Config>)
        [[nodiscard]] RenderResult<StorageTypeId> registerStorage(
            const typename Storage::Descriptor::Config& config = {}
        ) noexcept;

        template <class Storage> requires std::is_void_v<typename Storage::Descriptor::Config>
        [[nodiscard]] RenderResult<StorageTypeId> registerStorage() noexcept;

        // Registers a Handler factory and its typed methods, without constructing the Handler.
        // Repeating a compatible (type, key) reuses the identity and preserves selected bindings.
        template <class Handler>
        [[nodiscard]] RenderResult<HandlerId> registerHandler(std::string_view key = {}) noexcept;

        // Runs once, blocking the calling thread. Drains accepted work and replies before returning.
        [[nodiscard]] RenderResult<void> run() noexcept;

        // Thread-safe stop request. It does not join the rendering thread or cancel accepted GPU work.
        void stop() noexcept;

        // Thread-safe individual counters; the aggregate is not a coherent frame snapshot.
        [[nodiscard]] RenderServerStats stats() const noexcept;

    private:
        [[nodiscard]] RenderResult<FeatureId>
        registerFeature(
            const FeatureDescriptor&,
            const FeatureGraphOps&,
            std::span<const StorageFactory>,
            std::span<runtime_detail::HandlerRecord>,
            std::span<const CommandBinding>
        ) noexcept;

        [[nodiscard]] RenderResult<StorageTypeId> registerStorage(StorageFactory, ConfigValue) noexcept;
        [[nodiscard]] RenderResult<HandlerId>
        registerHandler(runtime_detail::HandlerRecord, std::span<const CommandBinding>) noexcept;

        [[nodiscard]] bool pagedInstances() const noexcept;
        [[nodiscard]] RenderResult<FeatureId> findFeature(const FeatureDescriptor&) const noexcept;
        [[nodiscard]] RenderResult<CommandId> findCommand(const CommandDescriptor&) const noexcept;
        struct Impl;
        explicit RenderServer(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::render
