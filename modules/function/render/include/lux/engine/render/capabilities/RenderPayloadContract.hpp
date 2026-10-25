#pragma once

#include <cstdint>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <string>

namespace lux::render
{
    enum class ERenderPayloadUse : std::uint8_t
    {
        CONFIGURATION,
        EFFECT_INPUT,
        FRAME_INPUT,
        RESOURCE_INPUT
    };

    enum class ERenderPayloadLifetime : std::uint8_t
    {
        MOVED_VALUE,
        UNTIL_RESPONSE
    };

    // Schema/version describe meaning and encoding. cppType() qualifies same-process C++ input;
    // it is neither a serialization format nor proof of binary compatibility across arbitrary modules.
    struct RenderPayloadContract final
    {
        std::string schema;
        std::uint32_t version = 1;
        // Own the spelling: a query result may outlive the registering module or Server.
        std::string cpp_type_name;
        std::uint64_t cpp_type_hash = 0;
        ERenderPayloadUse use = ERenderPayloadUse::RESOURCE_INPUT;
        ERenderPayloadLifetime lifetime = ERenderPayloadLifetime::UNTIL_RESPONSE;
        bool required = true;
        bool changes_graph = false;
        std::string description;

        void setCppType(cxx::TypeToken type) noexcept
        {
            cpp_type_name = type.name();
            cpp_type_hash = type.hash();
        }

        [[nodiscard]] cxx::TypeToken cppType() const noexcept
        {
            return {cpp_type_hash, cpp_type_name};
        }

        bool operator==(const RenderPayloadContract&) const noexcept = default;
    };
} // namespace lux::render
