#pragma once

#include <lux/engine/system/SystemInstanceId.hpp>
#include <lux/engine/system/description/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::system::detail
{
    struct SystemDependencyOrdinalEdge final
    {
        std::size_t before{};
        std::size_t after{};
    };

    enum class ESystemDependencyOrderError : std::uint8_t
    {
        INVALID_EDGE,
        CYCLE,
        ALLOCATION_FAILURE,
    };

    [[nodiscard]] LUX_ENGINE_SYSTEM_DESCRIPTION_PUBLIC
        lux::cxx::expected<std::vector<std::size_t>, ESystemDependencyOrderError>
        deterministicSystemOrder(
            std::span<const SystemInstanceId> instances,
            std::span<const SystemDependencyOrdinalEdge> edges
        ) noexcept;
} // namespace lux::system::detail
