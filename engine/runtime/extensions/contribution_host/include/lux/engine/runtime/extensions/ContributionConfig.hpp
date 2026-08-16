#pragma once

#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstdint>

namespace lux::runtime
{
    struct ContributionConfig final
    {
        std::uint32_t schema_version{0u};
        lux::cxx::SharedBytes<> bytes;
    };

    enum class EActivationPersistence : std::uint8_t
    {
        TRANSIENT,
        SCENE,
        PROJECT
    };
}
