#pragma once

#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/PassContract.hpp>
#include <span>
#include <string>

namespace lux::toolchain
{
    // Cold-only boundary. SPIRV-Cross types remain private to the toolchain.
    [[nodiscard]] cxx::expected<void, std::string> validatePassSpirv(
        std::span<const std::uint32_t> words,
        const rdesc::PassShaderContract& contract
    ) noexcept;

    struct PassShaderModule
    {
        std::span<const std::uint32_t> words;
    };

    // Complete program validation uses the same canonical slots for shared stage resources.
    [[nodiscard]] cxx::expected<void, std::string> validatePassShaders(
        std::span<const PassShaderModule> modules,
        const rdesc::PassShaderContract& contract,
        std::uint32_t required_stages
    ) noexcept;
} // namespace lux::toolchain
