#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/MaterialEnums.hpp>
#include <lux/engine/material/MaterialCompileFailure.hpp>
#include <lux/engine/material/ShaderIR.hpp>

#include <cstdint>

namespace lux::material
{
    class MaterialGraph;

    // Fully owned domain compilation result; shader backend and device objects are absent.
    struct MaterialIR final
    {
        lux::shadergen::ShaderIR shader;
        lux::rdesc::ELightingTechnique shading_model{lux::rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS};
        lux::rdesc::EAlphaMode alpha_mode{lux::rdesc::EAlphaMode::OPAQUE_SURFACE};
        float alpha_cutoff{0.5F};
        bool double_sided{};
        std::uint64_t combined_fingerprint{};
    };

    // Validate and lower the graph using the module's single domain algorithm.
    // Disconnected values remain authorable; a compiled material requires one surface output.
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC cxx::expected<MaterialIR, MaterialCompileFailure>
    lowerMaterial(const MaterialGraph&) noexcept;
} // namespace lux::material
