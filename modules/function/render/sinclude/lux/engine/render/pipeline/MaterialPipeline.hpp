#pragma once
#include <lux/engine/render/pipeline/ShaderPermutation.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/core/MaterialFamily.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/function/visibility.h>

#include <lux/cxx/container/SparseSet.hpp>

#include <array>
#include <cstdint>

namespace lux::render
{
    /**
     * @brief Maps shading models to ShaderFeatureMask and pipeline handles.
     *
     * Phase 4: Primary storage is map-based (keyed by EShadingModel) with
     * family-level default pipelines.  Pipeline lookup: model-specific
     * override → family default → kInvalidPipelineHandle.
     */
    class LUX_FUNCTION_PUBLIC MaterialPipeline
    {
    public:
        MaterialPipeline();

        // ----- Family-level pipeline API -----
        void setFamilyPipeline(ELightingTechnique family, GraphicsPipelineHandle handle) noexcept;
        [[nodiscard]] GraphicsPipelineHandle getFamilyPipeline(ELightingTechnique family) const noexcept;

        // ----- EShadingModel-based API -----
        ShaderFeatureMask getMaterialFeatures(EShadingModel model) const noexcept;
        void setMaterialFeatures(EShadingModel model, ShaderFeatureMask features) noexcept;
        void setMaterialPipelineHandle(EShadingModel model, GraphicsPipelineHandle handle) noexcept;
        [[nodiscard]] GraphicsPipelineHandle getMaterialPipelineHandle(EShadingModel model) const noexcept;
        [[nodiscard]] bool hasDedicatedPipeline(EShadingModel model) const noexcept;

    private:
        // Family-level default pipelines (1 per family)
        std::array<GraphicsPipelineHandle, kLightingTechniqueCount> family_pipelines_;

        // Per-shading-model overrides and feature masks
        lux::cxx::SparseSet<uint16_t, GraphicsPipelineHandle> model_overrides_;
        lux::cxx::SparseSet<uint16_t, ShaderFeatureMask>      model_features_;
    };

} // namespace lux::render

