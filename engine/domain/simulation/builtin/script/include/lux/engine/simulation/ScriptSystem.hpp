#pragma once

#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/SimulationClock.hpp>
#include <lux/engine/simulation/SimulationSystemDescription.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptApiCapability.hpp>
#include <lux/engine/simulation/scripting/ScriptEndpointBridge.hpp>
#include <lux/engine/simulation/scripting/ScriptTimeEndpoint.hpp>
#include <lux/engine/simulation/ScriptRuntimeInput.hpp>
#include <lux/engine/simulation/script_system/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace lux::simulation::script
{
    struct ResolvedScriptArtifact final
    {
        const lux::script::ScriptArtifact* artifact{};
        void* lease{};
        void (*release)(void*) noexcept{};
    };

    struct ScriptArtifactResolver final
    {
        void* context{};
        bool (*resolve)(void*, const lux::asset::AssetId&, ResolvedScriptArtifact&) noexcept{};
    };

    struct ScriptRuntimeLimits final
    {
        std::size_t failure_capacity{};
        std::size_t instance_capacity{};
        std::size_t continuation_capacity{};
        std::size_t continuation_capacity_per_instance{};
        std::size_t awaitable_capacity{};
        std::size_t resume_queue_capacity{};
        std::size_t max_resume_payload_bytes{};
        std::size_t resumes_per_stable_point{};
        std::size_t next_step_wait_capacity{};
        std::size_t simulation_delay_capacity{};
        std::size_t event_wait_capacity{};
        std::size_t external_completion_capacity{};
    };

    struct ScriptRuntimeStats final
    {
        std::size_t configured_mounts{};
        std::size_t pending_mounts{};
        std::size_t mount_backing_bytes{};
        std::size_t method_backing_bytes{};
        std::size_t binding_backing_bytes{};
        std::size_t mount_feedback_backing_bytes{};
        std::size_t active_instances{};
        std::size_t active_continuations{};
        std::size_t active_awaitables{};
        std::size_t active_event_waiters{};
        std::size_t event_waiter_high_water{};
        std::size_t event_waiter_dispatch_visits{};
        std::size_t instance_cleanup_event_waiter_visits{};
        std::size_t instance_cleanup_awaitable_visits{};
        std::size_t instance_cleanup_continuation_visits{};
        std::size_t resume_queue_depth{};
        std::size_t resume_queue_high_water{};
        std::size_t next_step_waits{};
        std::size_t simulation_delay_waits{};
        std::size_t external_completion_queue_depth{};
        std::size_t external_completion_queue_high_water{};
        std::size_t external_completion_capacity_failures{};
        std::uint64_t sync_invocations{};
        std::uint64_t step_invocations{};
        std::uint64_t backend_resume_calls{};
        std::uint64_t suspensions_admitted{};
        std::uint64_t event_occurrences{};
        std::uint64_t event_route_claim_lookups{};
        std::uint64_t event_payload_copy_bytes{};
        std::uint64_t completion_capability_constructions{};
        std::size_t result_write_pins{};
        std::size_t deferred_awaitable_releases{};
        std::size_t awaitable_record_bytes{};
        std::size_t event_waiter_record_bytes{};
        std::size_t awaitable_reserved_slots{};
        std::size_t awaitable_storage_bytes{};
        std::size_t external_ticket_storage_bytes{};
        // Cumulative assembly work, including rejected preflights; not instance resource counts.
        std::uint64_t assembly_configuration_slot_visits{};
        std::uint64_t assembly_endpoint_count_visits{};
    };

    namespace detail
    {
        struct ScriptAttachment final
        {
            std::uint32_t mount_slot{};
        };
    }

    enum class EScriptSystemError : std::uint8_t
    {
        INVALID_INPUT,
        DUPLICATE_BACKEND_KIND,
        DUPLICATE_ENDPOINT,
        SCRIPT_CAPABILITY_ID_COLLISION,
        SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER,
        CAPACITY_EXCEEDED,
        ASSET_NOT_RESIDENT,
        INVALID_ASSET,
        SYMBOL_NOT_FOUND,
        SCRIPT_ENDPOINT_NOT_FOUND,
        SCRIPT_CAPABILITY_NOT_FOUND,
        SCRIPT_CAPABILITY_SCHEMA_MISMATCH,
        SCRIPT_EVENT_NOT_FOUND,
        SCRIPT_EVENT_SCHEMA_MISMATCH,
        SIGNATURE_MISMATCH,
        SCOPE_MISMATCH,
        BACKEND_NOT_AVAILABLE,
        BACKEND_FAILURE,
        ENDPOINT_CONNECTION_FAILURE,
        INVOCATION_FAILURE,
        ALLOCATION_FAILURE,
        ENDPOINT_BUSY,
        CONTINUATION_CAPACITY_EXCEEDED,
        INSTANCE_CONTINUATION_CAPACITY_EXCEEDED,
        AWAITABLE_CAPACITY_EXCEEDED,
        RESUME_QUEUE_FULL,
        SHUT_DOWN,
    };

    struct ScriptSystemFailure final
    {
        EScriptSystemError error{EScriptSystemError::INVALID_INPUT};
        ScriptMountId mount;
        lux::script::ScriptSymbolId symbol{};
        std::int32_t status{};
    };

    enum class EScriptLifecycleAdmission : std::uint8_t { ALLOW, RETIRE_ONLY };

    enum class EScriptMountState : std::uint8_t
    {
        INACTIVE, CONSTRUCTING, INITIALIZED, ACTIVE, RETIRING, FAULTED,
    };

    enum class EScriptMountSubmissionState : std::uint8_t
    {
        NONE, ACCEPTED, ACTIVATED, REJECTED, CANCELLED,
    };

    struct ScriptMountStatus final
    {
        ScriptMountId id;
        std::uint64_t revision{};
        EScriptMountState state{EScriptMountState::INACTIVE};
        ScriptInstanceId instance;
        ScriptInstanceScope scope;
        bool reclaimed{true};
        ScriptInstanceId retired_instance;
        std::uint64_t submission{};
        EScriptMountSubmissionState submission_state{EScriptMountSubmissionState::NONE};
        ScriptInstanceScope submitted_scope;
        EScriptSystemError submission_error{EScriptSystemError::INVALID_INPUT};
    };

    struct ScriptMountStatusCollection final
    {
        std::size_t written{};
        std::size_t remaining{};
    };

    // Cold composition helper. The loader includes unresolved configurations when computing its plan.
    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC
    lux::cxx::expected<ScriptRuntimeCapacityPlan, EScriptSystemError>
    planScriptRuntimeCapacity(std::span<const ScriptRuntimeMount> mounts) noexcept;

    class LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC ScriptSystem final
    {
    public:
        inline static constexpr auto Access = makeSystemAccessSpec<ComponentWrite<detail::ScriptAttachment>>();
        inline static constexpr std::array<HookPointSpec, 0U> Hooks{};
        inline static constexpr std::array<EventPointSpec, 0U> Events{};
        inline static constexpr SimulationSystemDescription Description{
            .type = {
                .canonical_name = "lux.simulation.ScriptSystem",
                .version = 1U
            },
            .hooks          = Hooks,
            .events         = Events
        };

        [[nodiscard]] static lux::cxx::expected<ScriptSystem, EScriptSystemError>
        create(
            const SimulationDescription &simulation,
            const ScriptRuntimeCapacityPlan &capacity,
            std::span<const ScriptRuntimeMount> mounts,
            ecs::Registry &registry,
            const SimulationClock &clock,
            ScriptRuntimeLimits limits,
            ScriptArtifactResolver artifacts,
            std::span<const ScriptApiCapabilityPublication> capabilities,
            std::span<const ScriptBackendDescriptor> backends,
            std::span<const ScriptHookEndpointDescriptor> hooks,
            std::span<const ScriptEventEndpointDescriptor> events,
            ScriptHostApi host = {},
            ScriptRealDelayEndpoint real_delay = {}) noexcept;

        ScriptSystem(ScriptSystem &&) noexcept;

        ScriptSystem &operator=(ScriptSystem &&) noexcept;

        ~ScriptSystem() noexcept;

        ScriptSystem(const ScriptSystem &) = delete;

        ScriptSystem &operator=(const ScriptSystem &) = delete;

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        prepare() noexcept;

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        executeStablePoint() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        processLifecycle(EScriptLifecycleAdmission admission = EScriptLifecycleAdmission::ALLOW) noexcept;
        void beginStableAdmission() noexcept;

        // Owner-only assembly. Success accepts the whole batch; lifecycle executes at existing boundaries.
        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        mountResolvedBatch(std::span<const ScriptRuntimeMount> mounts) noexcept;
        // Value snapshots; query does not acknowledge changes. Empty means the configuration is unknown.
        [[nodiscard]] lux::cxx::expected<std::optional<ScriptMountStatus>, EScriptSystemError>
        queryMountStatus(ScriptMountId id) const noexcept;
        // Only copied entries are acknowledged. Uncopied entries remain queued, including at shutdown.
        [[nodiscard]] lux::cxx::expected<ScriptMountStatusCollection, EScriptSystemError>
        collectMountStatusChanges(std::span<ScriptMountStatus> output) noexcept;


        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        shutdown() noexcept;

        [[nodiscard]] std::size_t activeInstanceCount() const noexcept;

        [[nodiscard]] std::size_t activeContinuationCount() const noexcept;

        [[nodiscard]] std::size_t activeAwaitableCount() const noexcept;

        // The execution owner reads live counters at safe points. Cross-thread readers use a published copy.
        [[nodiscard]] ScriptRuntimeStats stats() const noexcept;

        [[nodiscard]] std::span<const ScriptSystemFailure> failures() const noexcept;

    private:
        struct State;
        explicit ScriptSystem(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}
