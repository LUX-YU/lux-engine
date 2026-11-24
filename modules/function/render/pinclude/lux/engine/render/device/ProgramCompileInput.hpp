#pragma once

#include <optional>

#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
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
        NativeProgramId id;
        nri::Pipeline* pipeline;
        PassBindings bindings;
    };

    // The shader moves into native compilation. The remaining declarations are borrowed during linkage.
    struct RasterProgramRecipe final
    {
        std::span<const RasterPassTemplate::Input> arguments;
        const ParameterBinding& parameters;
        const RasterStreamTarget& target;
        rdesc::ShaderProgramArtifact artifact;
        const GraphicsState& graphics;
        std::span<const std::uint32_t> geometry_locations;
    };

    struct RasterProgramLink final
    {
        ProgramCompileInput input;
        RasterDescriptorLayout descriptors;
        std::vector<CompiledRasterProgram::ArgumentUses> argument_to_use;
        std::vector<VertexBinding> vertices;
        std::vector<VertexAttribute> geometry_attributes;
        std::uint32_t shared_texture_stages;
        std::uint32_t record_stages;
        rdesc::ShaderBlock record_layout;

        [[nodiscard]] CompiledRasterProgram finish(PreparedProgram) && noexcept;
    };

    [[nodiscard]] RenderResult<RasterProgramLink>
    linkRasterStreamProgram(const DeviceSession&, RasterProgramRecipe) noexcept;
} // namespace lux::render::detail
