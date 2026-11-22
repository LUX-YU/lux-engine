#pragma once

#include <optional>

#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>

namespace lux::render::detail
{
    struct ProgramCompileInput final
    {
        rdesc::ShaderProgramArtifact shader;
        PassBindings bindings;
        std::optional<RasterProgramKey> raster;
    };

    struct PreparedProgram final
    {
        CompiledPrograms::ProgramId id;
        nri::Pipeline* pipeline;
        PassBindings bindings;
    };
} // namespace lux::render::detail
