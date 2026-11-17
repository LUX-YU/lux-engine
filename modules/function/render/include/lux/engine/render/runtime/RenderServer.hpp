#pragma once

#include <lux/engine/render/capabilities/CapabilityDeclaration.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>
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

namespace lux::platform::ipc
{
    class RequesterId;
}

namespace lux::render
{
    struct ResourceRegistration;
    struct CapabilityImplementation;
    struct CapabilityDescription;
    class CapabilityRegistry;
    class CommandBinding;

    namespace runtime_detail
    {
        struct HandlerRecord;
        struct FeatureEntry;
    } // namespace runtime_detail

    struct RenderServerConfig final
    {
        std::string     channel_name;
        bool            validation = false;
        // In addition to the frame currently consumed by Render, Game may prepare/queue this many.
        // The Client reserves one of max_frames_ahead + 1 input carriers before preparing a frame.
        std::uint32_t   max_frames_ahead = 1;
        std::size_t     max_config_bytes = 1024 * 1024;
        // Scheduler availability does not imply a persistent staging allocation.
        std::optional<TransferOptions> uploads;
        std::uint32_t   frames_in_flight = 2;
        std::uint64_t   gpu_wait_ns = 1'000'000; // 0..1ms; IPC/CPU/stop are checked between waits.
        RenderErrorSink diagnostics;
        // Borrowed application wake hint, called on RenderThread after real reply publication.
        // It must not reenter RenderServer; its context remains valid until the rendering thread joins.
        void*           response_context = nullptr;
        void (*responses_available)(void*) noexcept = nullptr;
        // One immutable failure notification on RenderThread. Keep consuming replies and close the
        // Clients before stop(); device failure alone does not end the responder's lifetime.
        void* failure_context = nullptr;
        void (*execution_failed)(void*, RenderError) noexcept = nullptr;
    };

    struct RenderServerStats final
    {
        std::uint64_t ticks = 0, commands = 0, compiled_graphs = 0, submissions = 0;
        std::uint64_t validation_errors = 0, validation_warnings = 0;
        std::uint64_t storages_created = 0;
        std::uint64_t presentations = 0; // Successful QueuePresent calls, not measured display scanout.
    };

    // The application owns the rendering thread. only stop() and stats() may race with run().
    // Stop and join that thread before destruction; the borrowed ExecutionRuntime must outlive this Server.
    class LUX_RENDER_PUBLIC RenderServer final
    {
    public:
        using CreateResult = RenderResult<std::unique_ptr<RenderServer>>;

        // Creates the device, IPC responder and supported built-in declarations, but starts no thread.
        // Registration stores factories only. run() constructs selected Handlers; installation/Attach create Storage.
        [[nodiscard]] static CreateResult create(
            RenderServerConfig config,
            execution::ExecutionRuntime& execution,
            DeviceOptions device_options = {}
        ) noexcept;

        ~RenderServer() noexcept;
        RenderServer(const RenderServer&) = delete;
        RenderServer& operator=(const RenderServer&) = delete;

        // Setup-thread cold lookup. Runtime applications use ListCapabilities through IPC.
        [[nodiscard]] RenderResult<FeatureId>
        featureId(std::string_view implementation) const noexcept;

        // Extension authors include extensions/Registration.hpp for these template definitions.
        // Metadata and callback code must outlive the Server and all accepted work. Cold catalog
        // access belongs to the setup thread before run(), or after run() returns and is joined.
        // Runtime Handler/static-command installation is allowed only on the Server owner at a
        // frame boundary. Other running-thread catalog access is rejected. Query IDs through IPC.
        template <class Feature> [[nodiscard]] RenderResult<FeatureId>
        registerFeature() noexcept;

        template <class Implementation> [[nodiscard]] RenderResult<CapabilityImplementationId>
        registerCapabilityImplementation() noexcept;

        // Catalog changes are permitted before run(), or after run() has returned and joined all work.
        // Active execution is rejected rather than implicitly detaching another caller's resources.
        template <CapabilityConcept C> [[nodiscard]] RenderResult<CapabilityId> registerCapability() noexcept
        {
            return registerCapability(describeCapability<C>());
        }

        [[nodiscard]] RenderResult<CapabilityId>
        registerCapability(const CapabilityDescription&) noexcept;

        [[nodiscard]] RenderResult<CapabilityImplementationId>
        addCapabilityImplementation(CapabilityId capability, const CapabilityImplementation& declaration) noexcept;

        [[nodiscard]] RenderResult<CapabilityImplementationId>
        replaceCapabilityImplementation(CapabilityImplementationId previous, const CapabilityImplementation& declaration) noexcept;

        [[nodiscard]] RenderResult<void>
        removeCapabilityImplementation(CapabilityImplementationId id) noexcept;

        [[nodiscard]] RenderResult<void>
        removeCapability(CapabilityId id) noexcept;

        // Resource declaration and shared implementation metadata remain distinct at registration.
        [[nodiscard]] RenderResult<ResourceTypeId> registerResource(ResourceRegistration, ConfigValue = {}) noexcept;

        // Stateless typed functions use the same command directory without a dummy Handler owner.
        // Register on the setup thread, or at a Server-owner frame boundary. Query IDs through IPC.
        [[nodiscard]] RenderResult<void> registerCommands(std::span<const CommandBinding>) noexcept;

        // Installs the Resource definition, creation policy and its declaration-owned static command thunks.
        template <class Storage> requires(!std::is_void_v<typename Storage::Descriptor::Config>)
        [[nodiscard]] RenderResult<ResourceTypeId>
        registerResource(const typename Storage::Descriptor::Config& config = {}) noexcept;

        template <class Storage> requires std::is_void_v<typename Storage::Descriptor::Config>
        [[nodiscard]] RenderResult<ResourceTypeId>
        registerResource() noexcept;

        // Registers a Handler factory and its typed methods, without constructing the Handler.
        // Repeating a compatible (type, key) reuses the identity and preserves selected bindings.
        template <class Handler, class Bindings> [[nodiscard]] RenderResult<HandlerId>
        registerHandler(std::string_view key = {}) noexcept;

        // Runs once, blocking the calling thread. Drains accepted work and replies before returning.
        [[nodiscard]] RenderResult<void>
        run() noexcept;

        // Cold, before run(). Only this physical IPC Link may submit frames. Other Clients may
        // query and prepare unreferenced assets, but cannot mutate visible state outside a frame.
        [[nodiscard]] RenderResult<void>
        setFrameSource(platform::ipc::RequesterId requester) noexcept;

        // Thread-safe stop request. It does not join the rendering thread or cancel accepted GPU work.
        void stop() noexcept;

        // Thread-safe individual counters; the aggregate is not a coherent frame snapshot.
        [[nodiscard]] RenderServerStats stats() const noexcept;

    private:
        [[nodiscard]] RenderResult<FeatureId>
        registerFeature(runtime_detail::FeatureEntry) noexcept;

        [[nodiscard]] RenderResult<CapabilityImplementationId>
        registerCapabilityImplementation(
            CapabilityDescription,
            CapabilityImplementation,
            std::span<runtime_detail::FeatureEntry>,
            std::span<const ResourceRegistration>,
            std::span<runtime_detail::HandlerRecord>,
            std::span<const CommandBinding>
        ) noexcept;

        [[nodiscard]] RenderResult<HandlerId>
        registerHandler(runtime_detail::HandlerRecord, std::span<const CommandBinding>) noexcept;

        [[nodiscard]] bool pagedInstances() const noexcept;

        struct Impl;
        explicit RenderServer(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::render
