#pragma once

#include <lux/engine/toolchain/shader/PassValidation.hpp>
#include <vector>

namespace lux::toolchain
{
    struct RelocatedShader
    {
        std::vector<std::uint32_t> original, words;
        std::vector<PassDescriptorLocation> locations;
        std::uint32_t stage;
        bool operator==(const RelocatedShader&) const noexcept = default;
    };

    // Supported cooked format: one V/F/C entry point, direct pair decorations, SPIR-V 1.0–1.6.
    // Both original and final binaries receive SPIRV-Tools validation and schema reconciliation.
    [[nodiscard]] cxx::expected<RelocatedShader, std::string> relocatePassSpirv(
        std::span<const std::uint32_t> original,
        const rdesc::PassShaderContract& schema,
        std::span<const PassDescriptorLocation> final_locations,
        std::uint32_t expected_stage
    ) noexcept;
} // namespace lux::toolchain
