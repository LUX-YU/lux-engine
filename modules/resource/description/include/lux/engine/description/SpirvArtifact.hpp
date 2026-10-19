#pragma once

#include <lux/engine/description/ShaderInfo.hpp>

namespace lux::rdesc
{
    struct SpirvStage final
    {
        EShaderType stage = EShaderType::UNDEFINED;
        std::string entry;
        std::vector<std::uint32_t> words;
        bool operator==(const SpirvStage&) const noexcept = default;
    };

    // Cooked SPIR-V and exact entry-point ABI. Stages describe actual code, not a fixed graphics pair.
    // Owned bytes are the identity; no hash-only equality, compiler or parser in this header.
    struct SpirvArtifact final
    {
        std::vector<SpirvStage> stages;
        ShaderInfo interface;
        bool operator==(const SpirvArtifact&) const noexcept = default;
    };

} // namespace lux::rdesc
