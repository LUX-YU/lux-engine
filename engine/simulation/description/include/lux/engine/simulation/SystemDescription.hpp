#pragma once

#include <lux/engine/simulation/SystemEventDescription.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace lux::simulation
{
    struct SystemDescription final
    {
        std::string_view canonical_name;
        std::uint32_t version{};
        std::string_view configuration_schema_name;
        std::uint32_t configuration_schema_version{};
        std::span<const std::string_view> capabilities;
        std::span<const SystemHookPoint> hooks;
        std::span<const SystemEventDescription> events;
    };

    [[nodiscard]] constexpr bool validSystemDescription(
        const SystemDescription& description
    ) noexcept
    {
        if (description.canonical_name.empty() || description.version == 0U)
            return false;
        if (description.configuration_schema_name.empty() !=
            (description.configuration_schema_version == 0U))
        {
            return false;
        }
        for (std::size_t index{}; index < description.capabilities.size(); ++index)
        {
            if (description.capabilities[index].empty())
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (description.capabilities[index] ==
                    description.capabilities[previous])
                {
                    return false;
                }
            }
        }
        for (std::size_t index{}; index < description.hooks.size(); ++index)
        {
            const auto& hook = description.hooks[index];
            if (hook.name.empty() || hook.signature.returns.size() > 1U ||
                (hook.cardinality == ESystemHookCardinality::MULTI &&
                 !hook.signature.returns.empty()))
            {
                return false;
            }
            const auto valid_type = [](const auto& type) constexpr noexcept
            {
                return type.type_id != 0U && !type.canonical_name.empty() &&
                    type.type_id == lux::script::scriptSemanticTypeId(
                        type.canonical_name
                    );
            };
            for (const auto& parameter : hook.signature.parameters)
            {
                if (!valid_type(parameter))
                    return false;
            }
            for (const auto& result : hook.signature.returns)
            {
                if (!valid_type(result) ||
                    result.pass != lux::script::EScriptPassMode::VALUE)
                {
                    return false;
                }
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (hook.name == description.hooks[previous].name)
                {
                    return false;
                }
            }
        }
        for (std::size_t index{}; index < description.events.size(); ++index)
        {
            const auto& event = description.events[index];
            if (event.name.empty() || event.dispatch_hook.empty() ||
                !event.payload_cpp_type.isValid())
            {
                return false;
            }
            if (event.payload_schema_name.empty() !=
                (event.payload_schema_version == 0U))
            {
                return false;
            }
            bool hook_found{};
            for (const auto& hook : description.hooks)
                hook_found = hook_found || hook.name == event.dispatch_hook;
            if (!hook_found)
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (event.name == description.events[previous].name)
                    return false;
            }
        }
        return true;
    }
}
