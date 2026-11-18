#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <utility>

namespace lux::render::detail
{
    std::pair<std::uint32_t, std::uint32_t>
    RenderPlan::parameterRegion(std::uint32_t index, EParameterSource source) const noexcept
    {
        const auto found = std::ranges::find_if(
            descriptor_plan_->parameter_regions,
            [&](const auto& region) { return region.group == index && region.source == source; }
        );
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
        if (encoding_owner_)
        {
            // Custom encodings can own GPU objects. Their layouts stay leased until the same
            // accepted frontiers complete, and are released only after those objects are destroyed.
            encoding_owner_->deferRelease(
                [passes = std::move(recording_.passes), layouts = std::move(layout_uses_),
                 cache = program_cache_]() mutable noexcept
                {
                    passes.clear();
                    for (auto* layout : layouts)
                    {
                        cache->releaseLayout(layout);
                    }
                }
            );
            program_cache_->release(program_uses_);
        }
    }

    RenderPlan::RenderPlan(RenderPlan&& other) noexcept
        : instance_data_(std::move(other.instance_data_)), descriptor_plan_(std::move(other.descriptor_plan_)),
          device_(std::exchange(other.device_, nullptr)), device_addresses_(other.device_addresses_),
          output_description_(other.output_description_), program_cache_(std::exchange(other.program_cache_, nullptr)),
          encoding_owner_(std::exchange(other.encoding_owner_, nullptr)), program_uses_(std::move(other.program_uses_)),
          layout_uses_(std::move(other.layout_uses_)), resources_(std::move(other.resources_)),
          histories_(std::move(other.histories_)), condition_count_(other.condition_count_),
          recording_(std::move(other.recording_)), submissions_(std::move(other.submissions_)),
          imported_accesses_(std::move(other.imported_accesses_)),
          physical_resources_(std::move(other.physical_resources_)), pass_indices_(std::move(other.pass_indices_)),
          output_(other.output_), output_first_batch_(other.output_first_batch_),
          output_wait_stages_(other.output_wait_stages_), fixed_output_extent_(other.fixed_output_extent_)
    {
    }

    exec::task<RenderResult<void>> RenderPlan::prepareOperations(
        DeviceSession& session, const RenderGraph& graph, execution::CpuScheduler cpu
    ) noexcept
    {
        const auto owner_scheduler = co_await stdexec::read_env(stdexec::get_scheduler);
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        program_cache_ = &session.programs();
        encoding_owner_ = &session;
        std::uint32_t indexed_lists = 0, raster_lists = 0;
        std::vector<std::uint8_t> materialized;
        materialized.reserve(resources_.size());
        for (const auto& resource : resources_) materialized.push_back(resource.memory_used);
        std::vector<rdesc::ShaderProgramArtifact> programs(graph.programs().size());
        for (std::size_t i = 0; i < programs.size(); ++i)
        {
            const auto* selected = graph.programs()[i].find(session.enabledFeatures().shader_target, device_addresses_);
            if (selected)
            {
                programs[i] = *selected;
            }
        }
        for (auto& pass : recording_.passes)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kCommandCancelled});
            }
            const auto& source = graph.passes()[pass.source_pass];
            const auto& input = source.interface;
            auto& bindings = descriptor_plan_->passes[pass.source_pass];
            std::vector<nri::Format> colors;
            std::optional<PassRenderingInfo> rendering;
            if (const auto* scope = pass.rasterScope())
            {
                rendering.emplace();
                rendering->dynamic_depth_bias = scope->state_input.has_value();
                for (const auto& color : scope->colors)
                {
                    const auto& image =
                        std::get<GraphImageDesc>(resources_[color.access.version.resource.index].description);
                    colors.push_back(imageFormat(image.format).native);
                    rendering->samples = static_cast<std::uint8_t>(image.samples);
                }
                rendering->colors = colors;
                if (scope->depth)
                {
                    const auto& image =
                        std::get<GraphImageDesc>(resources_[scope->depth->access.version.resource.index].description);
                    rendering->depth = imageFormat(image.format).native;
                    rendering->samples = static_cast<std::uint8_t>(image.samples);
                }
            }
            if (input.program != kGraphInvalidIndex)
            {
                const auto* shader =
                    graph.programs()[input.program].find(session.enabledFeatures().shader_target, device_addresses_);
                if (!shader)
                {
                    co_return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                auto relocated = co_await (
                    stdexec::schedule(cpu) |
                    stdexec::then([&]() noexcept { return relocateShader(*shader, bindings.bindings); }) |
                    stdexec::upon_stopped(
                        []() noexcept -> RenderResult<rdesc::ShaderProgramArtifact>
                        { return cxx::unexpected(RenderError{kCpuStopped}); }
                    ) |
                    stdexec::continues_on(owner_scheduler) |
                    stdexec::upon_error(NativeDeliveryFailure<rdesc::ShaderProgramArtifact>{})
                );
                if (!relocated)
                {
                    co_return cxx::unexpected(relocated.error());
                }
                programs[input.program] = *relocated;
                RasterProgramKey key;
                key.shader = std::make_shared<const PreparedShader>(std::move(*relocated));
                const bool is_raster = source.kind == EGraphPassKind::GRAPHICS;
                if (is_raster)
                {
                    key.state = source.graphics.value_or(GraphicsState{});
                    if (rendering->dynamic_depth_bias)
                    {
                        key.state.depth_bias_constant = 1;
                    }
                    key.colors = colors;
                    key.depth = rendering->depth;
                    key.samples = rendering->samples;
                }
                auto layout = program_cache_->acquireLayout(bindings, static_cast<std::uint32_t>(key.colors.size()));
                if (!layout)
                {
                    co_return cxx::unexpected(layout.error());
                }
                cxx::scope_exit release_layout([&]() noexcept { program_cache_->releaseLayout(*layout); });
                const auto lookup = is_raster ? program_cache_->lookupRaster(key, **layout)
                                              : program_cache_->lookupCompute(key.shader, **layout);
                auto* program = lookup.hit();
                if (!program)
                {
                    if (stop.stop_requested())
                    {
                        co_return cxx::unexpected(RenderError{kCommandCancelled});
                    }
                    // key/layout belong to this coroutine operation and are frozen until native return.
                    // The worker sees native facts only; cache/Plan publication stays on the control owner.
                    auto compiled = co_await (
                        stdexec::schedule(session.nativeScheduler()) |
                        stdexec::then(
                            [&key, layout = bindings.layout, raster = is_raster, device = &session.device(),
                             core = &session.nriCore(), enabled = session.enabledFeatures(),
                             limits = &session.description(),
                             formats = session.formatSupport(),
                             vertex_limits =
                                 std::array{session.vertexStrideLimit(), session.vertexOffsetLimit()}]() noexcept
                            {
                                if (raster)
                                {
                                    nri::Pipeline* pipeline = nullptr;
                                    const auto created = createRasterProgram(
                                        *device, *core, *limits, enabled, formats, vertex_limits, *layout, key, pipeline
                                    );
                                    if (!created)
                                    {
                                        if (pipeline) core->DestroyPipeline(pipeline);
                                        return RenderResult<nri::Pipeline*>{cxx::unexpected(created.error())};
                                    }
                                    return RenderResult<nri::Pipeline*>{pipeline};
                                }
                                return compileCompute(*device, *core, enabled, *layout, key.shader->artifact);
                            }
                        ) |
                        stdexec::upon_error(NativeScheduleFailure<nri::Pipeline*>{}) |
                        stdexec::upon_stopped(
                            []() noexcept -> RenderResult<nri::Pipeline*>
                            { return cxx::unexpected(RenderError{kCommandCancelled}); }
                        ) |
                        stdexec::continues_on(owner_scheduler) |
                        stdexec::upon_error(NativeDeliveryFailure<nri::Pipeline*>{})
                    );
                    if (!compiled)
                    {
                        co_return cxx::unexpected(compiled.error());
                    }
                    program = is_raster ? program_cache_->publishRaster(std::move(key), lookup, *compiled)
                                        : program_cache_->publishCompute(std::move(key.shader), lookup, *compiled);
                }
                program_uses_.push_back(program);
                pass.pipeline = CompiledPrograms::native(*program);
            }
            else if (!bindings.private_bindings.empty())
            {
                auto layout = program_cache_->acquireLayout(bindings, static_cast<std::uint32_t>(colors.size()));
                if (!layout)
                {
                    co_return cxx::unexpected(layout.error());
                }
                layout_uses_.push_back(*layout);
            }
            PassCompileContext context{
                session,       input,          programs,     bindings.recordBindings(), rendering, graph.resources(),
                pass.pipeline, &indexed_lists, &raster_lists, materialized
            };
            auto encoded = source.work.compile(context);
            if (!encoded)
            {
                co_return cxx::unexpected(encoded.error());
            }
            pass.work = std::move(*encoded);
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
        const bool fixed_extent_mismatch = fixed_output_extent_[0] != 0 && (target.width != fixed_output_extent_[0] ||
                                                                            target.height != fixed_output_extent_[1]);
        if (fixed_extent_mismatch)
        {
            return false;
        }
        // Instance creation qualifies resolved extents and copy pitches against actual backing capacities.
        // Resize can retain this immutable plan while preparing a complete candidate before publication.
        return true;
    }

} // namespace lux::render::detail
