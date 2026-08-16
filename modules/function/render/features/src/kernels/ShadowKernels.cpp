/**
 * @file ShadowKernels.cpp
 * @brief Kernel descriptors for shadow rendering paths.
 *
 * Registers:
 *   "ShadowCull" — multi-slice shadow frustum cull + dispatch
 *   "ShadowDraw" — shadow indirect draw with bias-group lanes
 *
 * Contribution hooks:
 *   ShadowCull::contribute_arena — accumulates shadow slice count
 *
 * Also provides:
 *   - Replay functions for KernelCommand dispatch
 *   - DynamicPatch resolution via PatchFn
 *   - ShadowFrameExtData extension slot registration
 */

#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/renderer/features/BufferTransferSynchronization.hpp>
#include <lux/engine/render/graph/KernelReplayContext.hpp>
#include <lux/engine/render/graph/ProgramEmitter.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/resources/lighting/ShadowFrameExtData.hpp>
#include <lux/engine/render/resources/mesh/MeshInstanceExtData.hpp>
#include <lux/engine/render/resources/mesh/GpuDrivenMeshConsts.hpp>
#include <lux/engine/render/resources/mesh/MdcTable.hpp>            // MdcEntry variant select
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>        // EGeometryKind

#include <algorithm>
#include <cstring>

namespace lux::render
{
    // =========================================================================
    //  Shadow frame extension slot (§3: now managed by KernelRegistry)
    // =========================================================================

    FrameExtensionSlotId shadowFrameExtSlot() noexcept
    {
        static const FrameExtensionSlotId slot =
            KernelRegistry::instance().extSlotOf("shadow");
        return slot;
    }

} // namespace lux::render

namespace lux::render::kernels
{

    // =========================================================================
    //  Shadow sub-command IDs (used by KernelCommand emit/replay)
    // =========================================================================

    enum ShadowSubCmd : uint8_t
    {
        kUploadFrustums     = 0,
        kCullPushConstants  = 1,
        kDrawLaneSetup      = 2,
        kBindIndexBuffer    = 3,
    };

    // =========================================================================
    //  Shadow DynamicPatch sub-source IDs
    // =========================================================================

    enum ShadowPatchSource : uint16_t
    {
        kSliceCount = 0,
        kGroupCount = 1,
        kInstanceSlotCount = 2,
    };

    // =========================================================================
    //  Helper: get the ShadowFrameExtData from the extension slot
    // =========================================================================

    static const ShadowFrameExtData* getShadowExt(const RGFrameContext& ctx)
    {
        const auto slot = shadowFrameExtSlot();
        if (slot == kInvalidExtSlot || !ctx.ext_data[slot])
            return nullptr;
        return static_cast<const ShadowFrameExtData*>(ctx.ext_data[slot]);
    }

    // =========================================================================
    //  ShadowCull — emit (uses new KernelCommand for sub-commands)
    // =========================================================================

    static void emitShadowCullKernel(ProgramEmitter& e, uint32_t /*pi*/,
                                     const RGCompiledPass& cpass,
                                     const RGCompiledGraph& /*compiled*/)
    {
        if (cpass.pass->kernel_config.size < sizeof(MeshCullKernelConfig))
            return;

        const auto& cfg = cpass.pass->kernel_config.as<MeshCullKernelConfig>();
        const KernelTypeId kid = cpass.pass->kernel_id;

        // Sub-command: UploadFrustums
        uint32_t dummy = 0;
        e.emitKernelCommand(kid, kUploadFrustums,
                            &dummy, static_cast<uint16_t>(sizeof(dummy)));

        // FillBuffer (generic command — no change)
        struct {
            uint32_t     resource_idx;
            VkDeviceSize offset;
            VkDeviceSize size;
            uint32_t     fill_value;
        } fill{cfg.draw_count_rg.index, 0,
               static_cast<VkDeviceSize>(std::max(cfg.mdc_count, 1u)) * sizeof(uint32_t), 0};
        e.emit(ExecutionProgram::Command::EType::FillBuffer,
               &fill, static_cast<uint16_t>(sizeof(fill)));

        // Sub-command: CullPushConstants via the shared factory (SSOT with the
        // view fill + replay). slice_count / bias-group count default to 0 and are
        // filled per-dispatch by the shadow kernel; view_mdc_count addresses the
        // shadow MDC table.
        auto pc = makeShadowCullPushConstants(cfg.pass_mask, cfg.geometry_mask,
                                              cfg.extension_flags, cfg.view_mdc_count);
        e.emitKernelCommand(kid, kCullPushConstants,
                            &pc, static_cast<uint16_t>(sizeof(pc)));

        // Dispatch (generic command, patched via KernelPatch)
        struct { uint32_t x, y, z; } dispatch{1, 1, 1};
        const uint32_t cmd_idx = e.emit(
            ExecutionProgram::Command::EType::Dispatch,
            &dispatch, static_cast<uint16_t>(sizeof(dispatch)));

        ExecutionProgram::DynamicPatch patch{};
        patch.command_index     = cmd_idx;
        patch.data_field_offset = 0;
        patch.source            = ExecutionProgram::DynamicPatch::ESource::KernelPatch;
        // Encode: high 8 bits = kernel ID, low 8 bits = sub-source
        patch.source_param      = (static_cast<uint16_t>(kid) << 8) | kInstanceSlotCount;
        e.program.patches.push_back(patch);
    }

    // =========================================================================
    //  ShadowCull — arena contribution
    // =========================================================================

    static void shadowCullContributeArena(const RGPassDescription& pass_desc,
                                          ViewArenaContribution& accum)
    {
        if (pass_desc.kernel_config.size < sizeof(MeshCullKernelConfig))
            return;
        const auto& cfg = pass_desc.kernel_config.as<MeshCullKernelConfig>();
        accum.shadow_slice_count = std::max(accum.shadow_slice_count, cfg.max_slices);
    }

    // =========================================================================
    //  ShadowDraw — emit (uses new KernelCommand for lane setup)
    // =========================================================================

    static void emitShadowDrawKernel(ProgramEmitter& e, uint32_t /*pi*/,
                                     const RGCompiledPass& cpass,
                                     const RGCompiledGraph& /*compiled*/)
    {
        if (cpass.pass->kernel_config.size < sizeof(ShadowDrawKernelConfig))
            return;

        const auto& cfg = cpass.pass->kernel_config.as<ShadowDrawKernelConfig>();
        const uint32_t vmc = std::max(cfg.view_mdc_count, 1u);
        const KernelTypeId kid = cpass.pass->kernel_id;

        // Skinned-shadow variant select: pipeline_variants[0] = static
        // depth pipeline, [family_count] = skinned (_vp) variant. We bind the
        // variant matching each view-MDC's geometry kind so skinned shadows
        // follow the animated pose. family_count == 0 → legacy single pipeline.
        const auto&     variants     = cpass.render.pipeline_variants;
        const uint32_t  family_count = cfg.family_count;
        const MdcEntry* mdc_entries  = cfg.mdc_entries;

        // 索引缓冲绑定走本 kernel 的子命令,不再借用 L2 的领域定型 opcode
        // BindMeshBuffers(它让录制器去注册表 find<MeshResources>(),是 L2 反向
        // 认识 L3 的抽象泄露)。深度管线同样 clear() 掉顶点输入,故原先随同的
        // vkCmdBindVertexBuffers 一并删除 —— 绑了无人消费。
        //
        // 句柄为空 = 没有网格存储,此时也不会有可绘制的 view-MDC。
        if (cfg.index_buffers_rg == nullptr || cfg.index_buffer_count == 0u)
            return;

        uint32_t last_vidx = ~0u;
        std::uint16_t last_ibo_segment = ~std::uint16_t{0u};
        VkIndexType last_index_type = VK_INDEX_TYPE_MAX_ENUM;
        auto bindVariant = [&](uint32_t vidx) {
            if (vidx == last_vidx) return;
            if (vidx >= variants.size() || variants[vidx] == VK_NULL_HANDLE) return;
            struct { VkPipeline pipeline; VkPipelineLayout layout; }
                bp{variants[vidx], cpass.render.pipeline_layout};
            e.emit(ExecutionProgram::Command::EType::BindPipeline,
                   &bp, static_cast<uint16_t>(sizeof(bp)));
            last_vidx = vidx;
        };

        // Only iterate the active bias-group lanes. The count/indirect buffers
        // are sized to bias_group_count*view_mdc_count, so looping the full
        // kMaxShadowBiasGroups would read past the end (offsets the validation
        // layer flags as countBufferOffset > buffer size). A config that leaves
        // bias_group_count == 0 issues no draws (no shadow casters this frame).
        const uint32_t bgc = (cfg.bias_group_count < kMaxShadowBiasGroups)
                                 ? cfg.bias_group_count : kMaxShadowBiasGroups;
        for (uint32_t g = 0; g < bgc; ++g)
        {
            // Sub-command: DrawLaneSetup
            struct { uint32_t lane_id; uint32_t atlas_resolution; }
                lane_data{g, cfg.atlas_resolution};
            e.emitKernelCommand(kid, kDrawLaneSetup,
                                &lane_data, static_cast<uint16_t>(sizeof(lane_data)));

            for (uint32_t m = 0; m < vmc; ++m)
            {
                if (mdc_entries == nullptr || m >= cfg.view_mdc_count)
                    continue;
                const auto& mdc = mdc_entries[m];
                if (mdc.ibo_segment >= cfg.index_buffer_count)
                    continue;

                if (mdc.ibo_segment != last_ibo_segment ||
                    mdc.index_type != last_index_type)
                {
                    struct
                    {
                        uint32_t resource_idx;
                        uint32_t index_type;
                    } bind_idx{
                        cfg.index_buffers_rg[mdc.ibo_segment].index,
                        static_cast<uint32_t>(mdc.index_type)};
                    e.emitKernelCommand(
                        kid,
                        kBindIndexBuffer,
                        &bind_idx,
                        static_cast<uint16_t>(sizeof(bind_idx)));
                    last_ibo_segment = mdc.ibo_segment;
                    last_index_type = mdc.index_type;
                }

                // Only manage pipeline binds when skinned variants are registered
                // (family_count > 0). family_count == 0 emits NO BindPipeline, so
                // the pass-level pipeline set via setPipeline() stays bound —
                // i.e. "use the pass pipeline for everything" is guaranteed
                // structurally here. (This is why the old forcePassPipeline flag
                // was vestigial and has been removed.)
                if (family_count > 0u)
                {
                    const uint32_t vidx =
                        (mdc_entries != nullptr && m < cfg.view_mdc_count
                         && mdc_entries[m].geometry_kind == static_cast<uint8_t>(EGeometryKind::SkinnedMesh))
                            ? family_count : 0u;
                    bindVariant(vidx);
                }

                const uint32_t shadow_mdc = g * vmc + m;
                struct {
                    uint32_t     indirect_resource_idx;
                    uint32_t     count_resource_idx;
                    VkDeviceSize indirect_offset;
                    VkDeviceSize count_offset;
                    uint32_t     max_draws;
                    uint32_t     stride;
                } draw{cfg.indirect_rg.index, cfg.draw_count_rg.index,
                       static_cast<VkDeviceSize>(shadow_mdc)
                           * sizeof(VkDrawIndexedIndirectCommand),
                       static_cast<VkDeviceSize>(shadow_mdc) * sizeof(uint32_t),
                       1u, kIndirectCommandSize};
                e.emit(ExecutionProgram::Command::EType::DrawIndexedIndirectCount,
                       &draw, static_cast<uint16_t>(sizeof(draw)));
            }
        }
    }

    // =========================================================================
    //  Shadow Replay Functions (KernelDescriptor::ReplayFn)
    // =========================================================================

    static void replayShadowCullCommand(uint32_t sub_cmd, const void* data,
                                         uint16_t data_size, KernelReplayContext& ctx)
    {
        const auto* ext = getShadowExt(ctx.frame_ctx);

        switch (sub_cmd) {
        case kUploadFrustums:
        {
            if (!ext || !ext->frustum_data || ext->frustum_size == 0
                || ext->cull_ubo == VK_NULL_HANDLE)
                break;

            synchronizeBeforeBufferTransferWrites(
                ctx.cmd,
                std::array{ext->cull_ubo}
            );
            vkCmdUpdateBuffer(ctx.cmd, ext->cull_ubo, 0,
                              static_cast<VkDeviceSize>(ext->frustum_size),
                              ext->frustum_data);

            const VkDeviceSize kGroupMapOffset =
                static_cast<VkDeviceSize>(ext->slice_count) * 6u * 16u;
            if (ext->group_map_data && ext->group_map_size > 0)
            {
                vkCmdUpdateBuffer(ctx.cmd, ext->cull_ubo,
                                  kGroupMapOffset,
                                  static_cast<VkDeviceSize>(ext->group_map_size),
                                  ext->group_map_data);
            }

            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers    = &barrier;
            vkCmdPipelineBarrier2(ctx.cmd, &dep);
            break;
        }
        case kCullPushConstants:
        {
            MeshCullPushConstants pc{};
            if (data_size < sizeof(pc))
                break;
            std::memcpy(&pc, data, sizeof(pc));

            // Patch dynamic fields from extension data (slot_count + the per-frame
            // world-partition active-mask address; both rotate each frame).
            {
                const auto isl = meshInstanceExtSlot();
                if (isl != kInvalidExtSlot && ctx.frame_ctx.ext_data[isl])
                {
                    const auto* ext = static_cast<const MeshInstanceExtData*>(
                        ctx.frame_ctx.ext_data[isl]);
                    pc.slot_count       = ext->slot_count;
                    pc.active_mask_addr = ext->active_mask_addr;
                }
                else
                {
                    pc.slot_count       = 0;
                    pc.active_mask_addr = 0ull;
                }
            }
            if (ext)
            {
                pc.geometry_kind_count_or_slice_count = ext->slice_count;
                pc.buckets_per_geometry_or_bias_groups = ext->group_count;
            }

            if (pc.slot_count > 0 && ctx.current_layout != VK_NULL_HANDLE)
            {
                vkCmdPushConstants(ctx.cmd, ctx.current_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT,
                                   0, sizeof(pc), &pc);
            }
            break;
        }
        default:
            break;
        }
    }

    static void replayShadowDrawCommand(uint32_t sub_cmd, const void* data,
                                         uint16_t data_size, KernelReplayContext& ctx)
    {
        if (sub_cmd == kBindIndexBuffer)
        {
            struct
            {
                uint32_t resource_idx;
                uint32_t index_type;
            } bind_idx{};
            if (data_size < sizeof(bind_idx))
                return;
            std::memcpy(&bind_idx, data, sizeof(bind_idx));

            const VkBuffer ibo = ctx.resolveBuffer(bind_idx.resource_idx);
            if (ibo != VK_NULL_HANDLE)
                vkCmdBindIndexBuffer(
                    ctx.cmd,
                    ibo,
                    0,
                    static_cast<VkIndexType>(bind_idx.index_type));
            return;   // 不触碰 skip_next_draw —— 那是每条 lane 的状态
        }

        if (sub_cmd != kDrawLaneSetup)
            return;

        const auto* ext = getShadowExt(ctx.frame_ctx);

        struct { uint32_t lane_id; uint32_t atlas_resolution; } lane_data;
        if (data_size < sizeof(lane_data))
            return;
        std::memcpy(&lane_data, data, sizeof(lane_data));

        const uint32_t g = lane_data.lane_id;
        if (ext && g < ShadowFrameExtData::kMaxBiasGroups
            && ext->draw_lanes[g].active)
        {
            const auto& lane = ext->draw_lanes[g];

            const float res = static_cast<float>(lane_data.atlas_resolution);
            VkViewport vp{0.0f, 0.0f, res, res, 0.0f, 1.0f};
            vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
            vkCmdSetScissor(ctx.cmd, 0, 1, &lane.scissor);
            vkCmdSetDepthBias(ctx.cmd, lane.depth_bias, 0.0f, lane.slope_bias);
            ctx.skip_next_draw = false;
        }
        else
        {
            ctx.skip_next_draw = true;
        }
    }

    // =========================================================================
    //  Shadow Patch Resolution (KernelDescriptor::PatchFn)
    // =========================================================================

    static uint32_t resolveShadowPatch(uint16_t source_param,
                                        const RGFrameContext& frame_ctx)
    {
        switch (source_param) {
        case kSliceCount:
        {
            const auto* ext = getShadowExt(frame_ctx);
            const uint32_t sc = ext ? ext->slice_count : 0;
            return (sc + kCullDispatchWorkgroupSize - 1u) / kCullDispatchWorkgroupSize;
        }
        case kGroupCount:
        {
            const auto* ext = getShadowExt(frame_ctx);
            return ext ? ext->group_count : 0;
        }
        case kInstanceSlotCount:
        {
            const auto isl = meshInstanceExtSlot();
            uint32_t alive = 0;
            if (isl != kInvalidExtSlot && frame_ctx.ext_data[isl])
            {
                alive = static_cast<const MeshInstanceExtData*>(
                    frame_ctx.ext_data[isl])->slot_count;
            }
            return (alive + kCullDispatchWorkgroupSize - 1u) / kCullDispatchWorkgroupSize;
        }
        default:
            return 0;
        }
    }

} // namespace lux::render::kernels

// =============================================================================
//  Self-registration
// =============================================================================

LUX_REGISTER_KERNEL("ShadowCull",
    (lux::render::KernelDescriptor{
        .emit             = &lux::render::kernels::emitShadowCullKernel,
        .contribute_mesh  = nullptr,
        .contribute_arena = &lux::render::kernels::shadowCullContributeArena,
        .replay           = &lux::render::kernels::replayShadowCullCommand,
        .resolve_patch    = &lux::render::kernels::resolveShadowPatch,
        .ext_slot_name    = "shadow",
    }))

LUX_REGISTER_KERNEL("ShadowDraw",
    (lux::render::KernelDescriptor{
        .emit             = &lux::render::kernels::emitShadowDrawKernel,
        .contribute_mesh  = nullptr,
        .contribute_arena = nullptr,
        .replay           = &lux::render::kernels::replayShadowDrawCommand,
        .resolve_patch    = nullptr,
    }))
