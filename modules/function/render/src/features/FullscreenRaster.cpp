#include "FullscreenVertex.hpp"
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/FullscreenRaster.hpp>

namespace lux::render::detail
{
    RenderResult<RasterWork> fullscreenRaster(
        FeatureBuildContext& context,
        std::span<const std::uint32_t> fragment,
        std::span<const std::byte> reflection
    ) noexcept
    {
        rdesc::SpirvArtifact shader;
        shader.stages.push_back(
            {rdesc::EShaderType::VERTEX, "main", {std::begin(kFullscreenVertex), std::end(kFullscreenVertex)}}
        );
        shader.stages.push_back({rdesc::EShaderType::FRAGMENT, "main", {fragment.begin(), fragment.end()}});
        if (!rdesc::ShaderInfo::deserialize(reflection, shader.interface))
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        shader.interface.entry_points.push_back({"main", rdesc::EShaderType::VERTEX});
        RasterWork work;
        if (!shader.interface.push_constants.empty())
        {
            work.interface.parameters.values.resize(shader.interface.push_constants.front().size);
        }
        auto program = context.graph.addProgram(std::move(shader));
        if (!program)
        {
            return cxx::unexpected(program.error());
        }
        work.interface.program = *program;
        return work;
    }

} // namespace lux::render::detail
