#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <utility>

namespace lux::render::detail
{
    RenderPlan::RenderPlan(nri::Device& device, const nri::CoreInterface& nri_core, TargetDesc target) noexcept
        : device_(&device), nri_core_(&nri_core), target_(target)
    {
    }

    RenderPlan::~RenderPlan() noexcept
    {
        if (!device_)
        {
            return;
        }
        for (auto pipeline : pipelines_)
        {
            nri_core_->DestroyPipeline(pipeline);
        }
        for (auto* layout : layouts_)
        {
            nri_core_->DestroyPipelineLayout(layout);
        }
    }

    RenderPlan::RenderPlan(RenderPlan&& other) noexcept
        : descriptor_plan_(std::move(other.descriptor_plan_)), device_(std::exchange(other.device_, nullptr)),
          nri_core_(other.nri_core_), target_(other.target_), layouts_(std::move(other.layouts_)),
          resources_(std::move(other.resources_)), histories_(std::move(other.histories_)),
          condition_count_(other.condition_count_), recording_(std::move(other.recording_)),
          submissions_(std::move(other.submissions_)), imported_accesses_(std::move(other.imported_accesses_)),
          pass_indices_(std::move(other.pass_indices_)), pipelines_(std::move(other.pipelines_)),
          output_(other.output_), resizable_output_(other.resizable_output_)
    {
    }

    bool RenderPlan::compatible(const TargetDesc& target) const noexcept
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
        // Instance creation qualifies resolved extents and copy pitches against actual backing capacities.
        // Resize can retain this immutable plan while preparing a complete candidate before publication.
        return true;
    }

} // namespace lux::render::detail
