#pragma once

#include <lux/engine/render/resource/material/Data.hpp>
#include <lux/engine/render/resource/mesh/Data.hpp>
#include <lux/engine/description/Image.hpp>

namespace lux::render
{
    // One concrete built-in mesh use. Forward uses the selected output format; Shadow can use per-section bias.
    struct MeshPipelineUse final
    {
        MeshId mesh;
        bool front_counter_clockwise = true;
        bool section_depth_bias = false;
        rdesc::ETextureFormat forward_format = rdesc::ETextureFormat::RGBA8_UNORM;
        bool operator==(const MeshPipelineUse&) const noexcept = default;
    };

    // An empty selection releases the declared uses for this program and purpose.
    struct MaterialMeshPipelines final
    {
        MaterialProgramId program;
        rdesc::EMaterialPurpose purpose;
        std::vector<MeshPipelineUse> uses;
    };
}
