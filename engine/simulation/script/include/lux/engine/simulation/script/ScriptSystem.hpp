#pragma once

#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/SystemDescription.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/script/ScriptBackend.hpp>
#include <lux/engine/simulation/script/ScriptEndpointBridge.hpp>
#include <lux/engine/simulation/script/ScriptSystemDescription.hpp>
#include <lux/engine/simulation/script/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::simulation::script
{
    namespace detail
    {
        struct ScriptAttachment final
        {
            lux::world::WorldObjectId object;
        };
    }

    struct ScriptSystemCapacities final
    {
        std::size_t instances{};
        std::size_t prepared_methods{};
        std::size_t hook_buckets{};
        std::size_t event_buckets{};
        std::size_t handlers{};
        std::size_t failures{};
        std::size_t mutations{};
    };

    enum class EScriptSystemError : std::uint8_t
    {
        INVALID_INPUT,
        DUPLICATE_BACKEND_KIND,
        DUPLICATE_ENDPOINT,
        CAPACITY_EXCEEDED,
        ASSET_NOT_RESIDENT,
        INVALID_ASSET,
        SYMBOL_NOT_FOUND,
        ENDPOINT_NOT_FOUND,
        SIGNATURE_MISMATCH,
        SCOPE_MISMATCH,
        BACKEND_NOT_AVAILABLE,
        BACKEND_FAILURE,
        WORLD_OBJECT_NOT_RESOLVED,
        ENDPOINT_CONNECTION_FAILURE,
        INVOCATION_FAILURE,
        ALLOCATION_FAILURE,
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
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.simulation.ScriptSystem",
            .version        = 1U,
            .hooks          = Hooks,
            .events         = Events
        };

        [[nodiscard]] static lux::cxx::expected<ScriptSystem, EScriptSystemError>
        create(
            const SimulationDescription &simulation,
            const ScriptSystemDescription &description,
            ecs::Registry &registry,
            ScriptSystemCapacities capacities,
            ResidentScriptResolver assets,
            ScriptWorldResolver world,
            std::span<const ScriptBackendDescriptor> backends,
            std::span<const ScriptHookEndpointDescriptor> hooks,
            std::span<const ScriptEventEndpointDescriptor> events,
            ScriptHostApi host = {}) noexcept;

        ScriptSystem(ScriptSystem &&) noexcept;
        
        ScriptSystem &operator=(ScriptSystem &&) noexcept;

        ~ScriptSystem() noexcept;

        ScriptSystem(const ScriptSystem &) = delete;

        ScriptSystem &operator=(const ScriptSystem &) = delete;

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        prepare() noexcept;

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        flushMutations() noexcept;

        [[nodiscard]] lux::cxx::expected<void, EScriptSystemError>
        shutdown() noexcept;

        [[nodiscard]] std::size_t 
        activeInstanceCount() const noexcept;

        [[nodiscard]] std::span<const ScriptSystemFailure> 
        failures() const noexcept;

    private:
        struct State;
        explicit ScriptSystem(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}
