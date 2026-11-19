#pragma once

#include <filesystem>
#include <lux/engine/description/ShaderProgramArtifact.hpp>

namespace lux::shader::detail
{
    enum class EDxilConstantLayout : std::uint8_t { VERIFIED, REFLECTION_UNAVAILABLE, LAYOUT_MISMATCH };

    [[nodiscard]] EDxilConstantLayout
    verifyDxilConstants(const std::filesystem::path&, const rdesc::ShaderProgramArtifact&) noexcept;
}
