#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace lux::render
{
    template <class T>
    concept CpuPreparationValue = std::is_trivially_copyable_v<T> && std::same_as<T, std::remove_cv_t<T>> &&
        !std::is_array_v<T>;

    template <CpuPreparationValue T> struct PreparedCpuValue final
    {
        T value;
        std::uint64_t revision;
    };
} // namespace lux::render
