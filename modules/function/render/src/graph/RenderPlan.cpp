#include <lux/engine/render/graph/ShaderBindingPlan.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <utility>
#include <lux/engine/render/graph/CompiledPrograms.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/cxx/core/scope_exit.hpp>

namespace lux::render::detail
{
    std::pair<std::uint32_t, std::uint32_t> RenderPlan::parameterRegion(std::uint32_t index) const noexcept
    {
        const auto found =
            std::ranges::find(descriptor_plan_->parameter_regions, index, &ShaderBindingPlan::ParameterRegion::group);
        if (found == descriptor_plan_->parameter_regions.end())
        {
            return {};
        }
        const auto& region = *found;
        return {region.offset, region.layout.size};
    }

    RenderPlan::RenderPlan(nri::Device& device, EffectOutputDesc output) noexcept
        : device_(&device), output_description_(output)
    {
    }

    RenderPlan::~RenderPlan() noexcept
    {
        if (program_cache_) program_cache_->release(program_uses_, layout_uses_);
    }

    RenderPlan::RenderPlan(RenderPlan&& other) noexcept
        : descriptor_plan_(std::move(other.descriptor_plan_)), device_(std::exchange(other.device_, nullptr)),
          output_description_(other.output_description_), program_cache_(std::exchange(other.program_cache_, nullptr)),
          program_uses_(std::move(other.program_uses_)), layout_uses_(std::move(other.layout_uses_)),
          resources_(std::move(other.resources_)),
          histories_(std::move(other.histories_)), condition_count_(other.condition_count_),
          recording_(std::move(other.recording_)), submissions_(std::move(other.submissions_)),
          imported_accesses_(std::move(other.imported_accesses_)),
          physical_resources_(std::move(other.physical_resources_)), pass_indices_(std::move(other.pass_indices_)),
          output_(other.output_), output_first_batch_(other.output_first_batch_),
          output_wait_stages_(other.output_wait_stages_),
          fixed_output_extent_(other.fixed_output_extent_)
    {
    }

    exec::task<RenderResult<void>>
    RenderPlan::preparePrograms(DeviceSession& session, const RenderGraph& graph, execution::CpuScheduler cpu) noexcept
    {
        program_cache_ = &session.programs();
        for (auto& pass : recording_.passes)
        {
            const auto& work = graph.passes()[pass.source_pass].work;
            auto& bindings = descriptor_plan_->passes[pass.source_pass];
            const auto* raster = std::get_if<RasterWork>(&work);
            const auto* fixed = raster ? std::get_if<RasterWork::FixedProgram>(&raster->program) : nullptr;
            const auto* compute = std::get_if<ComputeWork>(&work);
            const auto* input = fixed ? &fixed->interface : compute ? &compute->interface : nullptr;
            if (!input)
            {
                if (std::holds_alternative<TransferWork>(work) && !bindings.private_bindings.empty())
                {
                    auto acquired = program_cache_->acquireLayout(bindings);
                    if (!acquired) co_return cxx::unexpected(acquired.error());
                    if (std::ranges::find(layout_uses_, *acquired) == layout_uses_.end())
                        layout_uses_.push_back(*acquired);
                    else program_cache_->releaseLayout(*acquired);
                }
                continue;
            }
            auto relocated = relocateShader(graph.programs()[input->program], bindings.bindings);
            if (!relocated) co_return cxx::unexpected(relocated.error());
            RasterProgramKey key;
            key.shader = std::move(*relocated);
            if (fixed)
            {
                key.state = fixed->graphics;
                if (raster->state_input) key.state.depth_bias_constant = 1;
                for (const auto& color : raster->colors)
                {
                    const auto& image = std::get<GraphImageDesc>(
                        resources_[color.access.version.resource.index].description
                    );
                    key.colors.push_back(imageFormat(image.format).native);
                    key.samples = image.samples;
                }
                if (raster->depth)
                {
                    const auto& image = std::get<GraphImageDesc>(
                        resources_[raster->depth->access.version.resource.index].description
                    );
                    key.depth = imageFormat(image.format).native;
                    key.samples = image.samples;
                }
            }
            auto* pipeline = fixed ? program_cache_->acquireRaster(key, bindings)
                                   : program_cache_->acquireCompute(key.shader, bindings);
            if (!pipeline)
            {
                auto layout = program_cache_->acquireLayout(bindings, static_cast<std::uint32_t>(key.colors.size()));
                if (!layout) co_return cxx::unexpected(layout.error());
                cxx::scope_exit release_layout([&]() noexcept { program_cache_->releaseLayout(*layout); });
                auto compiled = co_await (stdexec::schedule(cpu) | stdexec::then([&]() noexcept
                {
                    return fixed ? compileRasterProgram(session.device(), session.nriCore(), session.enabledFeatures(),
                        session.formatSupport(), {session.vertexStrideLimit(), session.vertexOffsetLimit()},
                        **layout, key)
                        : compileCompute(session.device(), session.nriCore(), session.enabledFeatures(),
                            **layout, key.shader);
                }) | stdexec::upon_stopped([]() noexcept -> RenderResult<nri::Pipeline*>
                { return cxx::unexpected(RenderError{kCpuStopped}); }));
                if (!compiled) co_return cxx::unexpected(compiled.error());
                pipeline = fixed ? program_cache_->publishRaster(std::move(key), *layout, *compiled)
                    : program_cache_->publishCompute(std::move(key.shader), *layout, *compiled);
                release_layout.release();
            }
            if (std::ranges::find(program_uses_, pipeline) == program_uses_.end()) program_uses_.push_back(pipeline);
            else program_cache_->release({&pipeline, 1});
            if (fixed) std::get<RasterCommands>(pass.work).pipeline = pipeline;
            else std::get<ComputeCommands>(pass.work).pipeline = pipeline;
        }
        co_return RenderResult<void>{};
    }

    bool RenderPlan::compatible(const TargetDesc& target) const noexcept
    {
        const bool same_type = target.kind == output_description_.kind && target.format == output_description_.format &&
                               target.samples == output_description_.samples &&
                               target.sampled == output_description_.sampled;
        if (!same_type)
        {
            return false;
        }
        const bool fixed_extent_mismatch = fixed_output_extent_[0] != 0 &&
            (target.width != fixed_output_extent_[0] || target.height != fixed_output_extent_[1]);
        if (fixed_extent_mismatch) return false;
        // Instance creation qualifies resolved extents and copy pitches against actual backing capacities.
        // Resize can retain this immutable plan while preparing a complete candidate before publication.
        return true;
    }

} // namespace lux::render::detail
