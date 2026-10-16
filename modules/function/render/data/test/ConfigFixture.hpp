#pragma once

#include <lux/engine/render/data/ConfigValue.hpp>
#include <cstdlib>
#include <vector>

namespace config_test
{
    struct alignas(64) ForeignConfig final
    {
        std::vector<std::byte> bytes;
        bool* pin_alive = nullptr;
        unsigned* destroyed = nullptr;

        ForeignConfig(std::vector<std::byte> input, bool& alive, unsigned& count) noexcept
            : bytes(std::move(input)), pin_alive(&alive), destroyed(&count) {}

        ForeignConfig(ForeignConfig&& other) noexcept
            : bytes(std::move(other.bytes)), pin_alive(std::exchange(other.pin_alive, nullptr)),
              destroyed(std::exchange(other.destroyed, nullptr)) {}

        ~ForeignConfig() noexcept
        {
            if (destroyed)
            {
                if (!*pin_alive) std::abort();
                ++*destroyed;
            }
        }
    };
}

namespace lux::render
{
    template <> struct ConfigValueTraits<config_test::ForeignConfig>
    {
        static RenderResult<std::size_t> retainedBytes(const config_test::ForeignConfig& value) noexcept
        {
            return sizeof(value) + value.bytes.capacity();
        }
    };
}
