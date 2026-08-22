#include <lux/engine/render/renderer/features/water/WaterFeature.hpp>
#include <lux/engine/render/renderer/features/BufferTransferSynchronization.hpp>
#include <lux/engine/function/render/client/genops/FogOperation.ops.hpp>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace lux::render
{
    namespace
    {
        constexpr std::uint32_t kMaximumVkUpdateBytes = 65536u;
    }

    WaterFeature::WaterFeature(Config config) noexcept
        : RenderFeature(RenderFeature::Config{"Water"})
        , config_(config)
    {
        const auto maximum_from_update = static_cast<std::uint32_t>(
            (kMaximumVkUpdateBytes - sizeof(GpuHeader)) /
            sizeof(GpuSurface));
        config_.maximum_surfaces = std::clamp(
            config_.maximum_surfaces,
            1u,
            maximum_from_update);
        slots_.reserve(config_.maximum_surfaces);
        free_slots_.reserve(config_.maximum_surfaces);
        gpu_upload_.reserve(
            sizeof(GpuHeader) +
            sizeof(GpuSurface) * config_.maximum_surfaces);
    }

    Expected<void> WaterFeature::initAndAttachTo(RenderScene&)
    {
        auto& context = renderContext();
        auto& shaders = context.globalRegistry().must<ShaderResources>();
        const std::array requests{
            PipelineStageRequest{
                EBuiltinShader::TONEMAP_VERT,
                config_.vertex_shader},
            PipelineStageRequest{
                EBuiltinShader::WATER_FRAG,
                config_.fragment_shader}};
        auto stages = preparePipelineStages(shaders, requests);
        if (!stages)
            return lux::cxx::unexpected(stages.error());

        color_sampler_ = context.descriptorService().sampler(
            SamplerDesc::linearClamp());
        depth_sampler_ = context.descriptorService().sampler(
            SamplerDesc::nearestClamp());
        auto pipeline = makeFullscreenTemplate(
            "Water",
            8u + static_cast<std::uint32_t>(
                sizeof(FogFeature::RenderState)),
            false);
        // The domain merger owns runtime slots 0..2 (Global, Bindless and
        // Feature). Water's source set 1 is therefore relocated intact to
        // runtime slot 3. Keep the declared set count aligned with that
        // post-relocation layout; the source shader still deliberately uses
        // set 1 for its private inputs and set 2 for uTex.
        pipeline.descriptor_set_count = 4u;
        pipeline.resource_slot_map.push_back({
            EDescriptorSetSlot::Texture,
            1u,
        });
        pipeline.vertex_shader = stages->module(0u);
        pipeline.fragment_shader = stages->module(1u);
        auto registered = context.pipelineManager().registerGraphicsTemplate(
            pipeline, stages->infos());
        if (!registered)
            return lux::cxx::unexpected(registered.error());
        pipeline_ = *registered;
        const auto input = context.pipelineManager().
            templatePrivateSetLayout(pipeline_, 1u);
        if (!input)
            return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(1u);
        input_layout_ = input.layout;
        input_slot_ = input.runtime_slot;
        return {};
    }

    void WaterFeature::onDetachFromScene(RenderScene&)
    {
        slots_.clear();
        free_slots_.clear();
        gpu_upload_.clear();
        input_layout_ = VK_NULL_HANDLE;
        input_slot_ = 1u;
        color_sampler_ = VK_NULL_HANDLE;
        depth_sampler_ = VK_NULL_HANDLE;
        pipeline_ = kInvalidPipelineHandle;
    }

    bool WaterFeature::valid(RWaterSurfaceHandle handle) const noexcept
    {
        return !handle.isNull() && handle.index < slots_.size() &&
            slots_[handle.index].alive &&
            slots_[handle.index].generation == handle.gen;
    }

    float WaterFeature::coverageAt(
        const SurfaceSlot& slot,
        float scene_time) const noexcept
    {
        if (slot.transition_duration <= 0.0f)
            return slot.target_coverage;
        const float t = std::clamp(
            (scene_time - slot.transition_start) /
                slot.transition_duration,
            0.0f,
            1.0f);
        const float smooth = t * t * (3.0f - 2.0f * t);
        return std::lerp(
            slot.start_coverage,
            slot.target_coverage,
            smooth);
    }

    WaterSurfaceCreatedReply WaterFeature::createSurface(
        const WaterSurfaceDesc& surface) noexcept
    {
        std::uint32_t index = 0u;
        if (!free_slots_.empty())
        {
            index = free_slots_.back();
            free_slots_.pop_back();
        }
        else
        {
            if (slots_.size() >= config_.maximum_surfaces)
                return {{}, 1u};
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back(SurfaceSlot{});
        }

        auto& slot = slots_[index];
        slot.surface = surface;
        slot.alive = true;
        slot.retiring = false;
        slot.transition_start = renderScene().sceneTime();
        slot.transition_duration =
            static_cast<float>(surface.transition_milliseconds) * 0.001f;
        slot.start_coverage = 0.0f;
        slot.target_coverage = 1.0f;
        slot.coverage = 0.0f;
        return {
            RWaterSurfaceHandle{index, slot.generation},
            0u};
    }

    void WaterFeature::updateSurface(
        RWaterSurfaceHandle handle,
        const WaterSurfaceDesc& surface) noexcept
    {
        if (!valid(handle) || slots_[handle.index].retiring)
            return;
        slots_[handle.index].surface = surface;
    }

    void WaterFeature::destroySurface(
        RWaterSurfaceHandle handle) noexcept
    {
        if (!valid(handle))
            return;
        auto& slot = slots_[handle.index];
        if (slot.retiring)
            return;
        const float now = renderScene().sceneTime();
        slot.coverage = coverageAt(slot, now);
        slot.transition_start = now;
        slot.transition_duration = static_cast<float>(
            slot.surface.transition_milliseconds) * 0.001f;
        slot.start_coverage = slot.coverage;
        slot.target_coverage = 0.0f;
        slot.retiring = true;
    }

    void WaterFeature::onFrameBegin(const FeatureFrameContext&)
    {
        fog_ = {};
        for (const auto* feature : renderScene().features())
        {
            if (feature != nullptr &&
                feature->typeId() ==
                    kFogFeatureFactory.descriptor.type)
            {
                fog_ = static_cast<const FogFeature*>(feature)->renderState();
                break;
            }
        }
        rebuildGpuSnapshot(renderScene().sceneTime());
    }

    void WaterFeature::rebuildGpuSnapshot(float scene_time)
    {
        std::vector<GpuSurface> surfaces;
        surfaces.reserve(slots_.size());
        transitioning_surfaces_ = 0u;

        for (std::uint32_t index = 0u; index < slots_.size(); ++index)
        {
            auto& slot = slots_[index];
            if (!slot.alive)
                continue;
            slot.coverage = coverageAt(slot, scene_time);
            const bool transition_done =
                scene_time - slot.transition_start >=
                    slot.transition_duration;
            if (slot.retiring && transition_done &&
                slot.coverage <= 0.0f)
            {
                slot.alive = false;
                slot.retiring = false;
                slot.generation = slot.generation ==
                    std::numeric_limits<std::uint32_t>::max()
                    ? 1u
                    : slot.generation + 1u;
                free_slots_.push_back(index);
                continue;
            }
            if (!transition_done)
                ++transitioning_surfaces_;

            GpuSurface gpu{};
            std::copy_n(
                slot.surface.transform.basis_local,
                12u,
                gpu.basis_local);
            std::copy_n(
                slot.surface.transform.page_delta,
                3u,
                gpu.page_delta);
            gpu.half_rough_normal[0] = slot.surface.half_extent[0];
            gpu.half_rough_normal[1] = slot.surface.half_extent[1];
            gpu.half_rough_normal[2] = slot.surface.roughness;
            gpu.half_rough_normal[3] = slot.surface.normal_strength;
            std::copy_n(
                slot.surface.absorption_color,
                3u,
                gpu.absorption_distance);
            gpu.absorption_distance[3] =
                slot.surface.absorption_distance;
            gpu.scroll_wave[0] = slot.surface.normal_scroll_a[0];
            gpu.scroll_wave[1] = slot.surface.normal_scroll_a[1];
            gpu.scroll_wave[2] = slot.surface.normal_scroll_b[0];
            gpu.scroll_wave[3] = slot.surface.normal_scroll_b[1];
            gpu.coverage_time[0] = slot.surface.wave_scale;
            gpu.coverage_time[1] = slot.coverage;
            gpu.coverage_time[2] = scene_time;
            gpu.texture_seed_flags[0] =
                slot.surface.normal_texture.isNull()
                    ? 0xffffffffu
                    : slot.surface.normal_texture.index;
            gpu.texture_seed_flags[1] = slot.surface.transition_seed;
            gpu.texture_seed_flags[2] = slot.retiring ? 1u : 0u;
            surfaces.push_back(gpu);
        }

        visible_patches_ = static_cast<std::uint32_t>(surfaces.size());
        const GpuHeader header{
            static_cast<std::uint32_t>(surfaces.size()), {0u, 0u, 0u}};
        gpu_upload_.resize(
            sizeof(header) + sizeof(GpuSurface) * surfaces.size());
        std::memcpy(gpu_upload_.data(), &header, sizeof(header));
        if (!surfaces.empty())
        {
            std::memcpy(
                gpu_upload_.data() + sizeof(header),
                surfaces.data(),
                sizeof(GpuSurface) * surfaces.size());
        }
    }

    bool WaterFeature::canRebaseSceneOrigin(
        const std::int64_t origin_delta[3]) const noexcept
    {
        for (const auto& slot : slots_)
        {
            if (slot.alive && !canRebaseRenderPageDelta(
                    slot.surface.transform.page_delta,
                    origin_delta))
            {
                return false;
            }
        }
        return true;
    }

    void WaterFeature::rebaseSceneOrigin(
        const std::int64_t origin_delta[3]) noexcept
    {
        for (auto& slot : slots_)
        {
            if (slot.alive)
            {
                rebaseRenderPageDelta(
                    slot.surface.transform.page_delta,
                    origin_delta);
            }
        }
    }

    WaterStatsReply WaterFeature::stats() const noexcept
    {
        std::uint32_t resident = 0u;
        for (const auto& slot : slots_)
            resident += slot.alive ? 1u : 0u;
        return WaterStatsReply{
            resident,
            visible_patches_,
            transitioning_surfaces_,
            0u,
            static_cast<std::uint64_t>(slots_.capacity()) *
                sizeof(SurfaceSlot),
            sizeof(GpuHeader) +
                static_cast<std::uint64_t>(config_.maximum_surfaces) *
                    sizeof(GpuSurface)};
    }

    void WaterFeature::addPasses(RGBuilder& builder)
    {
        auto color = builder.referenceTexture("FogColor");
        auto depth = builder.referenceTexture(
            targetSlotName(TargetSlot::LINEAR_DEPTH));

        RGBufferDescription surface_description{};
        surface_description.size = sizeof(GpuHeader) +
            static_cast<std::uint64_t>(config_.maximum_surfaces) *
                sizeof(GpuSurface);
        surface_description.stride = sizeof(GpuSurface);
        surface_description.element_count = config_.maximum_surfaces;
        surface_description.usage =
            ERGBufferUsageBits::STORAGE |
            ERGBufferUsageBits::TRANSFER_DST;
        surface_description.memory_usage = ERGMemoryUsage::GPU_ONLY;
        const auto surfaces = builder.createBuffer(
            "WaterSurfaceBuffer", surface_description);

        builder.addPass("WaterSurfaceUpload", ERGPassType::TRANSFER)
            .write(surfaces, ERGBufferRole::STORAGE)
            .setKernelFn([this, surfaces](const PassRecordContext& record)
            {
                const VkBuffer buffer = record.resolveBufferHandle(surfaces);
                if (buffer == VK_NULL_HANDLE || gpu_upload_.empty())
                    return;
                synchronizeBeforeBufferTransferWrites(
                    record.cmd,
                    std::array{buffer}
                );
                vkCmdUpdateBuffer(
                    record.cmd,
                    buffer,
                    0u,
                    gpu_upload_.size(),
                    gpu_upload_.data());
            })
            .setKernel("WaterSurfaceUploadPass")
            .stage(ERenderStage::PostProcess);

        RGTextureDescription output_description =
            RGTextureDescription::Relative(
                1.0f,
                1.0f,
                renderScene().pipelineConfig().lit_color_format);
        output_description.usage =
            static_cast<ERGTextureUsageFlags>(
                ERGTextureUsageBits::COLOR_ATTACHMENT) |
            static_cast<ERGTextureUsageFlags>(
                ERGTextureUsageBits::SAMPLED);
        const auto output = builder.createTexture(
            "WaterColor", output_description);
        const auto descriptors = builder.createTransientDS(
            "WaterInputDS",
            input_layout_,
            {
                {0u, EDescriptorType::COMBINED_IMAGE_SAMPLER,
                 color, color_sampler_,
                 EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                {1u, EDescriptorType::COMBINED_IMAGE_SAMPLER,
                 depth, depth_sampler_,
                 EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                {2u, EDescriptorType::STORAGE_BUFFER, surfaces},
            });
        auto& context = renderContext();
        builder.addPass("WaterComposite", ERGPassType::GRAPHICS)
            .read(color, lux::render::ETextureRole::SAMPLED)
            .read(depth, lux::render::ETextureRole::SAMPLED)
            .read(surfaces, ERGBufferRole::STORAGE)
            .write(output, lux::render::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(pipeline_)
            .bindSceneDS()
            .bindTransientDS(input_slot_, descriptors)
            .bindImmutableDS(
                EDescriptorSetSlot::Texture,
                context.globalRegistry().descriptorSetOf<TextureResources>())
            .setKernelFn([this](const PassRecordContext& record)
            {
                vkCmdPushConstants(
                    record.cmd,
                    record.pipeline_layout,
                    record.pc_stage_flags,
                    8u,
                    sizeof(fog_),
                    &fog_);
                vkCmdDraw(record.cmd, 3u, 1u, 0u, 0u);
            })
            .setKernel("WaterCompositePass")
            .stage(ERenderStage::PostProcess);
    }
} // namespace lux::render
