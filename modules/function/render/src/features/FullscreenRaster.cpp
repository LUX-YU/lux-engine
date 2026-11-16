#include "FullscreenVertex.hpp"
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/FullscreenRaster.hpp>

namespace lux::render::detail
{
    RenderResult<RasterWork>
    fullscreenRaster(FeatureBuildContext& context, rdesc::ShaderProgramArtifact shader) noexcept
    {
        auto vertex = kFullscreenVertex.load(shader.target);
        if (shader.stages.empty() || vertex.stages.empty())
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        shader.stages.insert(shader.stages.begin(), vertex.stages.begin(), vertex.stages.end());
        shader.interface.entry_points.insert(
            shader.interface.entry_points.end(),
            vertex.interface.entry_points.begin(),
            vertex.interface.entry_points.end()
        );
        shader.interface.vertex_inputs = std::move(vertex.interface.vertex_inputs);
        RasterWork work;
        auto& fixed_work = work.fixed();
        if (!shader.interface.push_constants.empty())
        {
            fixed_work.interface.parameters.values.resize(shader.interface.push_constants.front().size);
        }
        auto program = context.graph.addProgram(std::move(shader));
        if (!program)
        {
            return cxx::unexpected(program.error());
        }
        fixed_work.interface.program = *program;
        return work;
    }

} // namespace lux::render::detail
