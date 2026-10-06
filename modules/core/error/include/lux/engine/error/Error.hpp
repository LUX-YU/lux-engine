#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <lux/cxx/algorithm/hash.hpp>

namespace lux::error
{
    using ErrorId = std::uint64_t;

    [[nodiscard]] constexpr ErrorId errorId(std::string_view name) noexcept
    {
        return cxx::algorithm::fnv1a(name);
    }

    struct Error final
    {
        ErrorId type{};
        std::array<std::uint64_t, 3> args{};
        bool operator==(const Error&) const noexcept = default;
    };
}
