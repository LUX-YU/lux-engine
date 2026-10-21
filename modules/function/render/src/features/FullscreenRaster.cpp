#include "FullscreenVertex.hpp"
#include <lux/engine/render/features/FeatureAssembly.hpp>
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

    std::size_t parameterLayoutBytes(const rdesc::ShaderBlock& layout) noexcept
    {
        auto bytes = layout.fields.capacity() * sizeof(rdesc::ShaderMember);
        for (const auto& field : layout.fields)
        {
            const auto begin = reinterpret_cast<std::uintptr_t>(&field.path);
            const auto data = reinterpret_cast<std::uintptr_t>(field.path.data());
            if (data < begin || data >= begin + sizeof(field.path))
            {
                bytes += field.path.capacity() + 1;
            }
        }
        return bytes;
    }
} // namespace lux::render::detail
