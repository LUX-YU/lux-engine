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

    // These declaration fields are borrowed only while linking the stream on the owner.
    struct RasterProgramRecipe final
    {
        std::span<const RasterPassTemplate::Input> arguments;
        const ParameterBinding& parameters;
        const RasterStreamTarget& target;
        const rdesc::ShaderProgramArtifact& artifact;
        const GraphicsState& graphics;
        std::span<const std::uint32_t> geometry_locations;
    };

    struct RasterProgramLink final
    {
        ProgramCompileInput input;
        RasterDescriptorLayout descriptors;
        std::vector<CompiledRasterStream::ArgumentUses> argument_to_use;
        std::vector<VertexBinding> vertices;
        std::vector<VertexAttribute> geometry_attributes;
        std::uint32_t shared_texture_stages;
        std::uint32_t record_stages;
        rdesc::ShaderBlock record_layout;

        [[nodiscard]] CompiledRasterStream::Program finish(PreparedProgram) && noexcept;
    };

    [[nodiscard]] RenderResult<RasterProgramLink>
    linkRasterStreamProgram(const DeviceSession&, RasterProgramRecipe) noexcept;
} // namespace lux::render::detail
