#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/commands/CommandBindingLocation.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
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
    struct ResourceRegistration;
    struct CapabilityDefinition;
    struct EffectImplementation;
    class CommandBinding;
    class ResourceRegistry;

    namespace runtime_detail
    {
        struct HandlerRecord;

        template <class Storage>
        concept ResourceStorage = requires
        {
            typename Storage::Descriptor::Config;
            { Storage::factory() } noexcept -> std::same_as<ResourceRegistration>;
        };

        template <class Storage>
        concept ConfiguredResourceStorage = ResourceStorage<Storage> &&
            (!std::is_void_v<typename Storage::Descriptor::Config>) &&
            requires(const typename Storage::Descriptor::Config& config) { ConfigValue::make(config); };

        template <class Handler>
        concept HandlerFactory = std::is_nothrow_destructible_v<Handler> && requires
        {
            requires std::same_as<decltype(Handler::create), Handler*(ResourceRegistry&) noexcept>;
            std::string(Handler::name);
        };

        template <class Bindings> struct HandlerBindings;

        template <class Bindings>
        concept HandlerBindingList = requires { &HandlerBindings<Bindings>::make; };
    } // namespace runtime_detail

    struct RenderServerConfig final
    {
        std::string     channel_name;
        bool            validation = false;
        std::size_t     max_config_bytes = 1024 * 1024;
        // Scheduler availability does not imply a persistent staging allocation.
        std::optional<TransferOptions> uploads;
        std::uint32_t   frames_in_flight = 2;
        std::uint64_t   gpu_wait_ns = 1'000'000; // 0..1 ms between idle GPU polls. IPC and CPU can wake earlier.
        RenderErrorSink diagnostics;
        // Borrowed application wake hint, called on RenderThread after real reply publication.
        // It must not reenter RenderServer; its context remains valid until the rendering thread joins.
        void*           response_context = nullptr;
        void (*responses_available)(void*) noexcept = nullptr;
        // One immutable failure notification on RenderThread. Accepted replies are settled before the responder closes.
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

    struct RenderDevelopmentStats final
    {
        std::uint64_t input_rebuilds = 0, input_rebuild_ns = 0, job_selection_ns = 0;
        std::uint64_t jobs_considered = 0, jobs_selected = 0, dependency_edges = 0;
        std::uint64_t rounds_opened = 0, slot_cpu_hold_ns = 0;
        std::uint64_t program_preparations_holding_slot = 0, program_preparation_ns = 0;
        // Storage and sampled ranges from dynamic raster lists in accepted batches, accumulated across rounds.
        std::uint64_t raster_buffer_ranges = 0, raster_buffer_bytes = 0, raster_image_ranges = 0;
        std::uint64_t raster_image_subresources = 0;
        std::uint64_t program_preparations = 0, discarded_preparations = 0;
        // Each growth is a new backing allocation made by a selection or topology vector.
        std::uint64_t job_selection_calls = 0, job_selection_growths = 0, job_topology_growths = 0;
        // External vertex and index ranges selected by raster streams in accepted batches.
        std::uint64_t raster_geometry_ranges = 0, raster_geometry_bytes = 0;
    };

    // The application owns the rendering thread. Only stop() and stats() may race with run().
    // Stop and join that thread before destruction; the borrowed ExecutionRuntime must outlive this Server.
    class LUX_RENDER_PUBLIC RenderServer final
    {
    public:
        using CreateResult = RenderResult<std::unique_ptr<RenderServer>>;

        // Opens the IPC responder and registers built-in declarations. CreateDeviceSession opens native devices.
        // Registration stores factories only. run() constructs selected Handlers; Resource owners create Storage.
        [[nodiscard]] static CreateResult create(RenderServerConfig config, execution::ExecutionRuntime&) noexcept;

        ~RenderServer() noexcept;
        RenderServer(const RenderServer&) = delete;
        RenderServer& operator=(const RenderServer&) = delete;

        // Setup lookup. Runtime applications use the discovery commands through IPC.
        [[nodiscard]] RenderResult<HandlerId> handlerId(cxx::basic_type_info, std::string_view key = {}) const noexcept;

        // Extension authors include extensions/Registration.hpp for these template definitions.
        // Metadata and callback code must outlive the Server and all accepted work. Serialize all
        // direct catalog calls during setup. run() seals registration and takes over the catalog.
        // Resource declaration and shared implementation metadata remain distinct at registration.
        [[nodiscard]] RenderResult<ResourceTypeId> registerResource(ResourceRegistration, ConfigValue = {}) noexcept;

        [[nodiscard]] RenderResult<void> registerEffectCapability(CapabilityDefinition) noexcept;
        [[nodiscard]] RenderResult<void> registerEffectImplementation(EffectImplementation) noexcept;
        [[nodiscard]] RenderResult<CommandBindingLocation> registerNodeEffectCommand(const CommandBinding&) noexcept;

        // Stateless typed functions use the same command directory without a dummy Handler owner.
        // Install these bindings during serialized setup. Query IDs through IPC while running.
        [[nodiscard]] RenderResult<void> registerCommands(std::span<const CommandBinding>) noexcept;

        // Installs the Resource definition, creation policy and its declaration-owned static command thunks.
        template <runtime_detail::ConfiguredResourceStorage Storage>
        [[nodiscard]] RenderResult<ResourceTypeId>
        registerResource(const typename Storage::Descriptor::Config& config = {}) noexcept;

        template <runtime_detail::ResourceStorage Storage> requires std::is_void_v<typename Storage::Descriptor::Config>
        [[nodiscard]] RenderResult<ResourceTypeId>
        registerResource() noexcept;

        // Registers a Handler factory and its typed methods, without constructing the Handler.
        // A repeated (type, key) is a conflict; references use handlerId without another factory payload.
        template <runtime_detail::HandlerFactory Handler, runtime_detail::HandlerBindingList Bindings>
        [[nodiscard]] RenderResult<HandlerId>
        registerHandler(std::string_view key = {}) noexcept;

        // Runs once, blocking the calling thread. Drains accepted work and replies before returning.
        [[nodiscard]] RenderResult<void>
        run() noexcept;

        // Thread-safe stop request. It does not join the rendering thread or cancel accepted GPU work.
        void stop() noexcept;

        // Thread-safe individual counters; the aggregate is not a coherent frame snapshot.
        [[nodiscard]] RenderServerStats stats() const noexcept;

        // Available when the SDK is built with LUX_RENDER_DEVELOPMENT_METRICS.
        [[nodiscard]] std::optional<RenderDevelopmentStats> developmentStats() const noexcept;

    private:
        [[nodiscard]] RenderResult<void> registerBuiltins() noexcept;
        [[nodiscard]] RenderResult<HandlerId>
        registerHandler(runtime_detail::HandlerRecord, std::span<const CommandBinding>) noexcept;


        struct Impl;
        explicit RenderServer(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::render
