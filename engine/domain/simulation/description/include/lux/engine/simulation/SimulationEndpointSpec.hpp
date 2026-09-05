#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/simulation/SimulationEndpointId.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lux::simulation
{
    enum class EEventRoute : std::uint8_t
    {
        SIMULATION_BROADCAST,
        ENTITY_TARGETED,
    };

    struct HookPointSpec final
    {
        HookPointId id;
        std::string_view diagnostic_name;
        lux::semantic::SignatureView signature;
        bool script_capable{true};
        bool stable_resume{};
        std::uint32_t contract_version{1U};
    };

    struct EventPointSpec final
    {
        EventPointId id;
        std::string_view diagnostic_name;
        HookPointId dispatch_hook;
        EEventRoute route{EEventRoute::SIMULATION_BROADCAST};
        lux::semantic::TypeId payload_type{};
        std::string_view payload_schema_name;
        std::uint32_t payload_schema_version{};
        bool owner_reproduction{};
    };

    namespace detail
    {
        template <class Value>
        inline constexpr bool kValidEndpointParameter = []
        {
            using Reference = std::remove_reference_t<Value>;
            using Base = std::remove_cv_t<Reference>;
            if constexpr (!lux::semantic::TypeDeclared<Base> ||
                          std::is_pointer_v<Base> ||
                          std::is_rvalue_reference_v<Value>)
            {
                return false;
            }
            else if constexpr (std::is_lvalue_reference_v<Value>)
            {
                return std::is_const_v<Reference>;
            }
            return true;
        }();

        template <class Value>
        [[nodiscard]] consteval lux::semantic::Type endpointParameter() noexcept
        {
            static_assert(kValidEndpointParameter<Value>);
            using Base = std::remove_cv_t<std::remove_reference_t<Value>>;
            constexpr auto pass = std::is_lvalue_reference_v<Value>
                ? lux::semantic::EValuePass::CONST_REF
                : lux::semantic::EValuePass::VALUE;
            return lux::semantic::makeType<Base>(pass);
        }

        template <class Signature>
        struct EndpointSignatureStorage;

        template <class... Parameters>
        struct EndpointSignatureStorage<void(Parameters...)> final
        {
            static_assert((kValidEndpointParameter<Parameters> && ...));

            inline static constexpr std::array<
                lux::semantic::Type,
                sizeof...(Parameters)> parameter_types{
                    endpointParameter<Parameters>()...};
            inline static constexpr std::array<lux::semantic::Type, 0U>
                return_types{};

            [[nodiscard]] static constexpr lux::semantic::SignatureView
            view() noexcept
            {
                return {parameter_types, return_types};
            }
        };

        template <class... Parameters>
        struct EndpointSignatureStorage<void(Parameters...) noexcept> final
            : EndpointSignatureStorage<void(Parameters...)>
        {
        };
    }

    template <class Signature>
    [[nodiscard]] consteval HookPointSpec makeHookPointSpec(
        HookPointId id,
        std::string_view diagnostic_name,
        bool script_capable = true,
        bool stable_resume = false,
        std::uint32_t contract_version = 1U
    ) noexcept
    {
        static_assert(std::is_function_v<Signature>);
        return {
            id,
            diagnostic_name,
            detail::EndpointSignatureStorage<Signature>::view(), script_capable, stable_resume, contract_version};
    }

    template <class Payload>
        requires lux::semantic::TypeDeclared<Payload>
    [[nodiscard]] consteval EventPointSpec makeEventPointSpec(
        EventPointId id,
        std::string_view diagnostic_name,
        HookPointId dispatch_hook,
        EEventRoute route,
        std::string_view payload_schema_name,
        std::uint32_t payload_schema_version,
        bool owner_reproduction = false
    ) noexcept
    {
        return {
            id,
            diagnostic_name,
            dispatch_hook,
            route,
            lux::semantic::makeType<Payload>().type_id,
            payload_schema_name,
            payload_schema_version, owner_reproduction};
    }

    [[nodiscard]] constexpr bool validHookPointSpec(
        const HookPointSpec& spec
    ) noexcept
    {
        if (!spec.id.valid() || spec.diagnostic_name.empty() ||
            !spec.signature.returns.empty() || spec.contract_version == 0U ||
            (spec.stable_resume && !spec.script_capable))
        {
            return false;
        }
        for (const auto& parameter : spec.signature.parameters)
        {
            if (!parameter.valid())
                return false;
        }
        return true;
    }

    [[nodiscard]] constexpr bool validEventPointSpec(
        const EventPointSpec& spec
    ) noexcept
    {
        return spec.id.valid() && !spec.diagnostic_name.empty() &&
            spec.dispatch_hook.valid() && spec.payload_type != 0U &&
            !spec.payload_schema_name.empty() &&
            spec.payload_schema_version != 0U &&
            (spec.route == EEventRoute::SIMULATION_BROADCAST ||
             spec.route == EEventRoute::ENTITY_TARGETED);
    }
}
