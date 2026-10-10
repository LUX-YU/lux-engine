#pragma once
#include <cstddef>
#include <functional>

namespace lux::render
{
    /**
     * @brief Combine a hash seed with another value.
     *
     * Uses the 64-bit Boost hash_combine variant with the golden ratio constant.
     * This is the canonical hash_combine implementation for the render module.
     */
    inline void hash_combine(std::size_t& seed, std::size_t v) noexcept
    {
        seed ^= v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    }

    /**
     * @brief Convenience: hash a value of type T and combine into seed.
     */
    template <typename T> inline void hash_one(std::size_t& seed, const T& x) noexcept
    {
        hash_combine(seed, std::hash<T>{}(x));
    }
} // namespace lux::render
