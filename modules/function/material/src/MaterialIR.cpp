#include <lux/engine/material/MaterialIR.hpp>

#include <bit>
#include <cmath>

namespace lux::material
{
    std::uint64_t computeFingerprint(const MaterialIR& ir) noexcept
    {
        auto fingerprint = shadergen::computeFingerprint(ir.shader);
        const auto mix = [&](std::uint64_t value) noexcept
        {
            fingerprint ^= value;
            fingerprint *= 1099511628211ULL;
        };
        mix(static_cast<std::uint32_t>(ir.shading_model));
        mix(static_cast<std::uint8_t>(ir.alpha_mode));
        if (ir.alpha_mode == rdesc::EAlphaMode::MASK)
        {
            mix(std::bit_cast<std::uint32_t>(ir.alpha_cutoff));
        }
        return fingerprint;
    }

    bool validateMaterialIR(const MaterialIR& ir) noexcept
    {
        const bool is_invalid_model = ir.shading_model > rdesc::ELightingTechnique::GRAPH;
        const bool is_invalid_alpha = ir.alpha_mode > rdesc::EAlphaMode::BLEND;
        const bool is_invalid_cutoff = !std::isfinite(ir.alpha_cutoff);
        const bool is_invalid_state = is_invalid_model || is_invalid_alpha || is_invalid_cutoff;
        if (is_invalid_state || !shadergen::validateShaderIR(ir.shader))
        {
            return false;
        }
        return ir.shader.fingerprint == shadergen::computeFingerprint(ir.shader) &&
               ir.combined_fingerprint == computeFingerprint(ir);
    }
} // namespace lux::material
