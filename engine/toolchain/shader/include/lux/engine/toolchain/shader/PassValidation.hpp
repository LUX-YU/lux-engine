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
} // namespace lux::toolchain
