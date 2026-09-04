#pragma once

#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/SimulationClock.hpp>
#include <lux/engine/simulation/SimulationSystemDescription.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptApiCapability.hpp>
#include <lux/engine/simulation/scripting/ScriptEndpointBridge.hpp>
#include <lux/engine/simulation/scripting/ScriptTimeEndpoint.hpp>
#include <lux/engine/simulation/ScriptSystemDescription.hpp>
#include <lux/engine/simulation/script_system/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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

    struct WorldObjectResolver final
    {
        void* context{};
        bool (*resolve)(void*, const lux::world::WorldObjectId&, ecs::Entity&) noexcept{};
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
    };

    struct ScriptRuntimeStats final
    {
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
        WORLD_OBJECT_NOT_RESOLVED,
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
            const ScriptSystemDescription &description,
            ecs::Registry &registry,
            const SimulationClock &clock,
            ScriptRuntimeLimits limits,
            ScriptArtifactResolver artifacts,
            WorldObjectResolver world,
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
        shutdown() noexcept;

        [[nodiscard]] std::size_t activeInstanceCount() const noexcept;

        [[nodiscard]] std::size_t activeContinuationCount() const noexcept;

        [[nodiscard]] std::size_t activeAwaitableCount() const noexcept;

        [[nodiscard]] ScriptRuntimeStats stats() const noexcept;

        [[nodiscard]] std::span<const ScriptSystemFailure> failures() const noexcept;

    private:
        struct State;
        explicit ScriptSystem(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}
