#include <lux/engine/render/vulkan/shader/Program.hpp>

namespace lux::render::vulkan
{
    NativeShaderProgram::NativeShaderProgram(
        NativeProgramIdentity identity,
        std::vector<DescriptorSetLayout> sets,
        PipelineLayout layout,
        std::vector<ShaderModule> modules,
        VPipeline pipeline
    ) noexcept
        : identity_(std::move(identity)), sets_(std::move(sets)), pipeline_layout_(std::move(layout)),
          modules_(std::move(modules)), pipeline_(std::move(pipeline))
    {
    }

    VkPipeline NativeShaderProgram::native() const noexcept
    {
        return std::visit([](const auto& pipeline) { return pipeline.native(); }, pipeline_);
    }
} // namespace lux::render::vulkan
