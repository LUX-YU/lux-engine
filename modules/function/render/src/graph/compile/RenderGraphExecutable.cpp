#include <lux/engine/render/extensions/graph/RenderGraph.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/graph/compile/BuiltinPass.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <utility>

namespace lux::render::detail
{
    std::pair<std::uint32_t, std::uint32_t>
    RenderGraphExecutable::parameterRegion(std::uint32_t index, EParameterSource source) const noexcept
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

    RenderGraphExecutable::RenderGraphExecutable(nri::Device& device, EffectOutputDesc output) noexcept
        : device_(&device), output_description_(output)
    {
    }

    RenderGraphExecutable::~RenderGraphExecutable() noexcept = default;

    RenderGraphExecutable::RenderGraphExecutable(RenderGraphExecutable&&) noexcept = default;

    NativeGraphPrograms::NativeGraphPrograms(DeviceSession& session, std::size_t slots) noexcept
        : session_(&session), slots_(slots)
    {
    }

    NativeGraphPrograms::NativeGraphPrograms(NativeGraphPrograms&& other) noexcept
        : session_(std::exchange(other.session_, nullptr)), slots_(std::move(other.slots_)),
          program_uses_(std::move(other.program_uses_)), layout_uses_(std::move(other.layout_uses_))
    {
    }

    NativeGraphPrograms::~NativeGraphPrograms() noexcept
    {
        if (!session_) return;
        auto& programs = session_->programs();
        if (!layout_uses_.empty())
        {
            session_->deferRelease([layouts = std::move(layout_uses_), &programs]() noexcept
            {
                for (auto* layout : layouts)
                {
                    programs.releaseLayout(layout);
                }
            });
        }
        programs.release(program_uses_);
    }

    exec::task<RenderResult<void>> RenderGraphExecutable::prepareOperations(
        DeviceSession& session,
        NativeGraphPrograms& native,
        const RenderGraph& graph,
        std::span<const GraphPassId> order,
        execution::CpuScheduler cpu
    ) noexcept
    {
        const auto owner_scheduler = co_await stdexec::read_env(stdexec::get_scheduler);
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        auto& program_cache = session.programs();
        std::uint32_t indexed_lists = 0, raster_lists = 0;
        std::vector<std::uint8_t> materialized;
        materialized.reserve(resources_.size());
        for (const auto& resource : resources_) materialized.push_back(resource.memory_used);
        for (auto& pass : recording_.passes)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kCommandCancelled});
            }
            const auto& source = graph.passes()[order[pass.binding_slot].index];
            const auto& input = source.interface;
            auto& bindings = descriptor_plan_->passes[pass.binding_slot];
            const rdesc::ShaderProgramArtifact* shader = nullptr;
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
                shader = graph.programs()[input.program].find(session.enabledFeatures().shader_target, device_addresses_);
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
                auto layout = program_cache.acquireLayout(bindings, static_cast<std::uint32_t>(key.colors.size()));
                if (!layout)
                {
                    co_return cxx::unexpected(layout.error());
                }
                cxx::scope_exit release_layout([&]() noexcept { program_cache.releaseLayout(*layout); });
                const auto lookup = is_raster ? program_cache.lookupRaster(key, **layout)
                                              : program_cache.lookupCompute(key.shader, **layout);
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
                    program = is_raster ? program_cache.publishRaster(std::move(key), lookup, *compiled)
                                        : program_cache.publishCompute(std::move(key.shader), lookup, *compiled);
                }
                native.program_uses_.push_back(program);
                native.slots_[pass.binding_slot].pipeline = CompiledPrograms::native(*program);
            }
            else if (!bindings.private_bindings.empty())
            {
                auto layout = program_cache.acquireLayout(bindings, static_cast<std::uint32_t>(colors.size()));
                if (!layout)
                {
                    co_return cxx::unexpected(layout.error());
                }
                native.layout_uses_.push_back(*layout);
            }
            native.slots_[pass.binding_slot].descriptors = bindings.recordBindings();
            bindings.layout = nullptr;
            PassCompileContext context{
                session, shader ? &shader->interface : nullptr, graph.resources(),
                &indexed_lists, &raster_lists, materialized
            };
            const auto append = [&]<class T>(RenderResult<T> result, ERecordKernel kernel, std::vector<T>& table)
                noexcept -> RenderResult<RecordBlock>
            {
                if (!result) return cxx::unexpected(result.error());
                const auto packet = static_cast<std::uint32_t>(table.size());
                table.push_back(std::move(*result));
                return RecordBlock{kernel, packet};
            };
            auto commands = std::visit(
                [&](const auto& command) noexcept -> RenderResult<RecordBlock>
                {
                    using Command = std::remove_cvref_t<decltype(command)>;
                    if constexpr (std::is_same_v<Command, RasterWork>)
                    {
                        return append(RasterPass::compile(command, context), ERecordKernel::RASTER, recording_.raster);
                    }
                    else if constexpr (std::is_same_v<Command, ComputeWork>)
                    {
                        return append(
                            ComputePass::compile(command, context), ERecordKernel::COMPUTE, recording_.compute
                        );
                    }
                    else
                    {
                        return append(
                            TransferPass::compile(command, context), ERecordKernel::TRANSFER, recording_.transfer
                        );
                    }
                },
                source.command
            );
            if (!commands)
            {
                co_return cxx::unexpected(commands.error());
            }
            pass.commands = std::move(*commands);
        }
        co_return RenderResult<void>{};
    }

    bool RenderGraphExecutable::compatible(const TargetDesc& target) const noexcept
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
