#include <lux/engine/render/RenderContextView.hpp>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>

#include <vector>

namespace lux::render
{
    Expected<ShaderHandle> RenderContextView::createBuiltinShaderModule(
        EBuiltinShader builtin,
        ShaderHandle configured
    )
    {
        return resolveShaderStage(
            ctx_->globalRegistry().must<ShaderResources>(),
            configured,
            builtin
        );
    }

    Expected<PreparedPipelineStages> RenderContextView::preparePipelineStages(
        std::span<const PipelineStageDesc> stages
    )
    {
        auto& shaders = ctx_->globalRegistry().must<ShaderResources>();

        std::vector<ShaderHandle> resolved;
        resolved.reserve(stages.size());
        for (const PipelineStageDesc& stage : stages)
        {
            auto handle = resolveShaderStage(
                shaders,
                stage.configured,
                stage.builtin
            );
            if (!handle)
                return lux::cxx::unexpected(handle.error());
            resolved.push_back(*handle);
        }
        return shaders.preparePipelineStages(resolved);
    }

    Expected<void> RenderContextView::createBuiltinShaderModules(
        std::span<const BuiltinShaderSlot> slots
    )
    {
        auto& shaders = ctx_->globalRegistry().must<ShaderResources>();
        for (const BuiltinShaderSlot& slot : slots)
        {
            if (slot.target == nullptr)
                continue;
            auto handle = resolveShaderStage(
                shaders,
                *slot.target,
                slot.builtin
            );
            if (!handle)
                return lux::cxx::unexpected(handle.error());
            *slot.target = *handle;
        }
        return {};
    }
} // namespace lux::render
