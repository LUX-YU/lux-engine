#include <lux/engine/render/graph/CompiledGraph.hpp>
#include <lux/engine/render/graph/LayoutPlan.hpp>
#include <utility>

namespace lux::render::detail
{
    CompiledGraph::CompiledGraph(nri::Device& device, const nri::CoreInterface& nri_core, TargetDesc target) noexcept
        : device_(&device), nri_core_(&nri_core), target_(target)
    {
    }

    CompiledGraph::~CompiledGraph() noexcept
    {
        if (!device_)
        {
            return;
        }
        for (auto pipeline : pipelines_)
        {
            nri_core_->DestroyPipeline(pipeline);
        }
        if (layout_)
        {
            nri_core_->DestroyPipelineLayout(layout_);
        }
    }

    CompiledGraph::CompiledGraph(CompiledGraph&& other) noexcept
        : descriptor_plan_(std::move(other.descriptor_plan_)), device_(std::exchange(other.device_, nullptr)),
          nri_core_(other.nri_core_), target_(other.target_), layout_(other.layout_),
          resources_(std::move(other.resources_)), frame_contributions_(std::move(other.frame_contributions_)),
          storage_groups_(std::move(other.storage_groups_)), passes_(std::move(other.passes_)),
          pass_indices_(std::move(other.pass_indices_)), pipelines_(std::move(other.pipelines_)),
          output_(other.output_), resizable_output_(other.resizable_output_)
    {
    }

    bool CompiledGraph::compatible(const TargetDesc& target) const noexcept
    {
        const bool same_type =
            target.kind == target_.kind && target.format == target_.format && target.samples == target_.samples;
        if (!same_type)
        {
            return false;
        }
        const bool resized = target.width != target_.width || target.height != target_.height;
        if (resized && !resizable_output_)
        {
            return false;
        }
        if (resized)
        {
            for (const auto& pass : passes_)
            {
                // An explicit image-to-buffer Copy retains its declared byte extent. There is no
                // implicit readback allocation to resize behind the author's resource description.
                if (pass.recipe == ERecipe::COPY_IMAGE_BUFFER && resources_[pass.source].target_sized)
                {
                    return false;
                }
            }
        }
        return true;
    }

} // namespace lux::render::detail
