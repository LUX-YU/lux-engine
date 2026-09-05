#pragma once

#include <lux/engine/function/script/ScriptEvent.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/scripting/ScriptEndpointBridge.hpp>
#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cctype>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>

namespace lux::simulation::script
{
    enum class EScriptEventSourceProjectionError : std::uint8_t
    {
        INVALID_SOURCE,
        INVALID_CODE_NAME,
        ENDPOINT_MISMATCH,
        PAYLOAD_MISMATCH,
        ALLOCATION_FAILURE,
    };

    namespace detail
    {
        [[nodiscard]] inline bool scriptEventCodeName(std::string_view value) noexcept
        {
            if (value.empty())
                return false;
            const auto first = static_cast<unsigned char>(value.front());
            if (std::isalpha(first) == 0 && value.front() != '_')
                return false;
            for (const char character : value.substr(1U))
            {
                const auto byte = static_cast<unsigned char>(character);
                if (std::isalnum(byte) == 0 && character != '_')
                    return false;
            }
            return true;
        }
    }

    [[nodiscard]] inline lux::cxx::expected<lux::script::ScriptEventSourceDescription,
                                            EScriptEventSourceProjectionError>
    describeScriptEventSource(
        SimulationEventView event,
        const lux::semantic::Layout& owned,
        std::string_view system_name = {},
        std::string_view event_name = {}
    ) noexcept
    {
        if (!event || !event.dispatchHook().scriptCapable())
        {
            return lux::cxx::unexpected<EScriptEventSourceProjectionError>(
                EScriptEventSourceProjectionError::INVALID_SOURCE
            );
        }
        const auto system = event.system();
        const auto projected_system_name = system_name.empty() ? system.instanceName() : system_name;
        const auto projected_event_name = event_name.empty() ? event.name() : event_name;
        if (!detail::scriptEventCodeName(projected_system_name) ||
            !detail::scriptEventCodeName(projected_event_name))
        {
            return lux::cxx::unexpected<EScriptEventSourceProjectionError>(
                EScriptEventSourceProjectionError::INVALID_CODE_NAME
            );
        }

        const bool is_payload_mismatch = owned.type_id != event.payloadType() ||
            owned.canonical_name != event.payloadSchemaName() || owned.abi_kind == 0U || owned.size == 0U ||
            owned.size > std::numeric_limits<std::uint32_t>::max() || owned.alignment == 0U ||
            owned.alignment > std::numeric_limits<std::uint32_t>::max() ||
            (owned.alignment & (owned.alignment - 1U)) != 0U || event.payloadSchemaHash() == 0U ||
            event.payloadSchemaVersion() == 0U;
        if (is_payload_mismatch)
        {
            return lux::cxx::unexpected<EScriptEventSourceProjectionError>(
                EScriptEventSourceProjectionError::PAYLOAD_MISMATCH
            );
        }

        try
        {
            return lux::script::ScriptEventSourceDescription{
                std::string(projected_system_name),
                std::string(projected_event_name),
                system.instanceId().value,
                event.id().value,
                event.route() == EEventRoute::SIMULATION_BROADCAST
                    ? lux::script::EScriptEventRoute::SIMULATION_BROADCAST
                    : lux::script::EScriptEventRoute::ENTITY_TARGETED,
                {
                    std::string(owned.canonical_name),
                    owned.type_id,
                    owned.abi_kind,
                    static_cast<std::uint32_t>(owned.size),
                    static_cast<std::uint32_t>(owned.alignment)
                },
                event.payloadSchemaHash(),
                event.payloadSchemaVersion(),
                event.dispatchHook().id().value,
                event.dispatchHook().contractHash(),
                event.dispatchHook().contractVersion()
            };
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected<EScriptEventSourceProjectionError>(
                EScriptEventSourceProjectionError::ALLOCATION_FAILURE
            );
        }
    }

    template <class Payload>
        requires lux::semantic::TypeDeclared<std::remove_cv_t<Payload>>
    [[nodiscard]] inline lux::cxx::expected<lux::script::ScriptEventSourceDescription,
                                            EScriptEventSourceProjectionError>
    describeScriptEventSource(
        SimulationEventView event,
        std::string_view system_name = {},
        std::string_view event_name = {}
    ) noexcept
    {
        return describeScriptEventSource(
            event,
            detail::eventPayloadLayout<std::remove_cv_t<Payload>>(),
            system_name,
            event_name
        );
    }

    [[nodiscard]] inline lux::cxx::expected<lux::script::ScriptEventSourceDescription,
                                            EScriptEventSourceProjectionError>
    projectScriptEventSource(
        SimulationEventView event,
        const ScriptEventEndpointDescriptor& endpoint,
        std::string_view system_name = {},
        std::string_view event_name = {}
    ) noexcept
    {
        if (!event)
        {
            return lux::cxx::unexpected<EScriptEventSourceProjectionError>(
                EScriptEventSourceProjectionError::INVALID_SOURCE
            );
        }
        const auto described = describeScriptEventSource(
            event,
            endpoint.payload_projection.owned_layout,
            system_name,
            event_name
        );
        if (!described)
            return lux::cxx::unexpected(described.error());

        const bool is_endpoint_mismatch = endpoint.system != event.system().instanceId() ||
            endpoint.event != event.id() || endpoint.route != event.route();
        if (is_endpoint_mismatch)
        {
            return lux::cxx::unexpected<EScriptEventSourceProjectionError>(
                EScriptEventSourceProjectionError::ENDPOINT_MISMATCH
            );
        }
        const auto& payload = described->payload;
        const bool is_payload_mismatch = endpoint.payload_projection.copy == nullptr ||
            endpoint.payload_type.type_id != payload.type_id ||
            endpoint.payload_type.canonical_name != payload.canonical_name ||
            endpoint.payload_type.pass != lux::semantic::EValuePass::CONST_REF;
        return is_payload_mismatch
            ? lux::cxx::unexpected(EScriptEventSourceProjectionError::PAYLOAD_MISMATCH)
            : described;
    }
}
