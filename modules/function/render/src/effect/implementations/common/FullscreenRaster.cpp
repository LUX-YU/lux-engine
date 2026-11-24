#include "FullscreenVertex.hpp"
#include <lux/engine/render/graph/GraphError.hpp>

#include <lux/engine/render/effect/implementations/common/FullscreenRaster.hpp>

namespace lux::render::detail
{
    RenderResult<RasterWork>
    fullscreenRaster(EffectCompileContext& graph, GraphProgram shaders) noexcept
    {
        if (shaders.targets.empty()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        auto parameter_bytes = UINT32_MAX;
        for (auto& shader : shaders.targets)
        {
            auto vertex = kFullscreenVertex.load(shader.target);
            if (shader.stages.empty() || vertex.stages.empty())
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            shader.stages.insert(shader.stages.begin(), vertex.stages.begin(), vertex.stages.end());
            shader.interface.entry_points.insert(
                shader.interface.entry_points.end(),
                vertex.interface.entry_points.begin(),
                vertex.interface.entry_points.end()
            );
            shader.interface.vertex_inputs = std::move(vertex.interface.vertex_inputs);
            const auto bytes = shader.interface.push_constants.empty() ? 0 : shader.interface.push_constants.front().size;
            parameter_bytes = std::min(parameter_bytes, bytes);
        }
        RasterWork work;
        auto& fixed_work = work.fixed();
        // Authors may fill the common byte prefix. Target-specific trailing padding is supplied by Compile.
        fixed_work.interface.parameters.values.resize(parameter_bytes);
        fixed_work.interface.parameters.zero_fill_tail = true;
        auto program = graph.addProgram(std::move(shaders));
        if (!program)
        {
            return cxx::unexpected(program.error());
        }
        fixed_work.interface.program = *program;
        return work;
    }

} // namespace lux::render::detail
