/**
 * @file MeshKernels.cpp
 * @brief Kernel descriptors for GPU-driven mesh geometry paths.
 *
 * Registers:
 *   EPassKernel::MeshCull  — frustum cull + dispatch
 *   EPassKernel::MeshDraw  — indirect draw lanes + shared index buffer bind
 *
 * Contribution hooks:
 *   MeshCull::contribute_arena — counts frustum UBO slots
 *   MeshDraw::contribute_mesh  — generates ordered MeshLane entries
 */

#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/renderer/features/BufferTransferSynchronization.hpp>
#include <lux/engine/render/graph/KernelReplayContext.hpp>
#include <lux/engine/render/graph/ProgramEmitter.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/resources/mesh/MdcTable.hpp>
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>   // EGeometryKind
#include <lux/engine/render/resources/mesh/GpuDrivenMeshConsts.hpp>
#include <lux/engine/render/resources/mesh/MeshInstanceExtData.hpp>

#include <algorithm>
#include <cstring>

namespace lux::render::kernels
{
    // =========================================================================
    //  Extension slot for mesh instance data
    // =========================================================================

    static const MeshInstanceExtData* getInstanceExt(const RGFrameContext& fc)
    {
        const auto slot = meshInstanceExtSlot();
        if (slot == kInvalidExtSlot) return nullptr;
        return static_cast<const MeshInstanceExtData*>(fc.ext_data[slot]);
    }

    // =========================================================================
    //  Patch sub-source IDs for MeshCull
    // =========================================================================

    enum MeshCullPatchSource : uint16_t
    {
        kSlotCountPatch = 0,   ///< instance slot count → dispatch group X
    };

    // =========================================================================
    //  MeshCull 的 kernel 局部子命令 id
    // =========================================================================

    enum MeshCullSubCmd : uint8_t
    {
        kMeshCullPushConstants = 0,
        kMeshCullUploadFrustum = 1,
    };

    // =========================================================================
    //  MeshDraw 的 kernel 局部子命令 id
    // =========================================================================

    enum MeshDrawSubCmd : uint8_t
    {
        kMeshDrawBindIndexBuffer = 0,
    };

    // =========================================================================
    //  Constants (shared with recorder)
    // =========================================================================

    // kIndirectCommandSize lives in graph/KernelDescriptor.hpp — shared with the
    // shadow kernels and the graph compiler. This TU has the complete Vulkan
    // type, so it is where the claim in that comment is actually checked:
    static_assert(kIndirectCommandSize == sizeof(VkDrawIndexedIndirectCommand),
                  "indirect-draw stride drifted from the Vulkan struct it mirrors");

    // =========================================================================
    //  MeshCull — emit
    // =========================================================================

    static void emitMeshCullKernel(ProgramEmitter& e, uint32_t /*pi*/,
                                   const RGCompiledPass& cpass,
                                   const RGCompiledGraph& /*compiled*/)
    {
        if (cpass.pass->kernel_config.size < sizeof(MeshCullKernelConfig))
            return;

        const auto& cfg = cpass.pass->kernel_config.as<MeshCullKernelConfig>();
        const KernelTypeId kid = cpass.pass->kernel_id;

        // 视锥上传同样走本 kernel 的子命令。原先的 L2 opcode UploadViewFrustum
        // 要解引用 frame_ctx.view->frustum_staging,是渲染图机器认识场景 View
        // (L4)的抽象泄露,还把长度写死成 6*16。现在 Renderer 把视锥打包成中性
        // 字节挂在 RGFrameContext 上,这里只按实际长度搬。
        uint32_t frustum_res = cfg.frustum_ubo_rg.index;
        e.emitKernelCommand(kid, kMeshCullUploadFrustum,
                            &frustum_res, static_cast<uint16_t>(sizeof(frustum_res)));

        struct {
            uint32_t     resource_idx;
            VkDeviceSize offset;
            VkDeviceSize size;
            uint32_t     fill_value;
        } fill{cfg.draw_count_rg.index, 0,
               static_cast<VkDeviceSize>(cfg.draw_list_count) * sizeof(uint32_t), 0};
        e.emit(ExecutionProgram::Command::EType::FillBuffer,
               &fill, static_cast<uint16_t>(sizeof(fill)));

        // VIEW-mode cull PC via the shared factory (single source of truth with
        // the shader's PC block + the shadow/replay fills). The factory sets
        // geometry_kind_count = kGeometryKindCount; a previous bug filled that
        // slot positionally with the draw-LANE (mdc) count, which silently culled
        // every instance whose kind >= lane count (a lone skinned instance, kind=1
        // mdc_count=1, rendered nothing). slot_count is patched per-frame below.
        //
        // 推送常量走**本 kernel 自己的子命令**,不再借用 L2 的通用 opcode
        // EType::CullPushConstants —— 那条 opcode 的录制器实现要读
        // MeshInstanceExtData 才能补上每帧轮换的 slot_count/active_mask_addr,
        // 等于让渲染图机器(L2)认识网格域数据(L3),是抽象泄露。搬进 kernel 后
        // 领域知识回到领域自己手里,L2 的 opcode 也随之整条删除。
        // (阴影侧早已是这个形状:ShadowKernels 的 kCullPushConstants。)
        auto pc = makeViewCullPushConstants(cfg.pass_mask, cfg.geometry_mask,
                                            cfg.extension_flags);
        e.emitKernelCommand(kid, kMeshCullPushConstants,
                            &pc, static_cast<uint16_t>(sizeof(pc)));

        if (cfg.dispatch_indirect_rg)
        {
            struct
            {
                uint32_t resource_idx;
                VkDeviceSize offset;
            } dispatch{
                cfg.dispatch_indirect_rg.index,
                cfg.dispatch_indirect_offset};
            e.emit(
                ExecutionProgram::Command::EType::DispatchIndirect,
                &dispatch,
                static_cast<uint16_t>(sizeof(dispatch)));
        }
        else
        {
            struct { uint32_t x, y, z; } dispatch{1, 1, 1};
            const uint32_t cmd_idx = e.emit(
                ExecutionProgram::Command::EType::Dispatch,
                &dispatch, static_cast<uint16_t>(sizeof(dispatch)));

            ExecutionProgram::DynamicPatch patch{};
            patch.command_index     = cmd_idx;
            patch.data_field_offset = 0;
            patch.source            = ExecutionProgram::DynamicPatch::ESource::KernelPatch;
            patch.source_param      = (static_cast<uint16_t>(kid) << 8) | kSlotCountPatch;
            e.program.patches.push_back(patch);
        }
    }

    // =========================================================================
    //  MeshCull — arena contribution
    // =========================================================================

    static void meshCullContributeArena(const RGPassDescription& /*pass_desc*/,
                                        ViewArenaContribution& accum)
    {
        // Called only for passes registered as "MeshCull" — always increment.
        ++accum.frustum_ubo_count;
    }

    // =========================================================================
    //  MeshCull — replay
    // =========================================================================

    /// 录制期推送视图剔除的常量块。emit 期烘好的是不随帧变的部分;slot_count 与
    /// 世界分区活跃掩码地址每帧轮换,只能在这里从 ext 数据补。
    static void replayMeshCullCommand(uint32_t sub_cmd, const void* data,
                                      uint16_t data_size, KernelReplayContext& ctx)
    {
        if (sub_cmd == kMeshCullUploadFrustum)
        {
            uint32_t resource_idx = 0;
            if (data_size < sizeof(resource_idx))
                return;
            std::memcpy(&resource_idx, data, sizeof(resource_idx));

            // 中性字节由组装方(Renderer)挂上,长度以实际为准 —— 旧实现写死
            // 6*16,一旦视锥布局变动就会静默截断/越读。
            const auto frustum = ctx.frame_ctx.view_frustum;
            if (frustum.empty())
                return;

            const VkBuffer buf = ctx.resolveBuffer(resource_idx);
            if (buf == VK_NULL_HANDLE)
                return;

            synchronizeBeforeBufferTransferWrites(
                ctx.cmd,
                std::array{buf}
            );
            vkCmdUpdateBuffer(ctx.cmd, buf, 0,
                              static_cast<VkDeviceSize>(frustum.size_bytes()),
                              frustum.data());

            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers    = &barrier;
            vkCmdPipelineBarrier2(ctx.cmd, &dep);
            return;
        }

        if (sub_cmd != kMeshCullPushConstants)
            return;

        MeshCullPushConstants pc{};
        std::memcpy(&pc, data,
                    std::min(sizeof(pc), static_cast<size_t>(data_size)));

        if (const auto* ext = getInstanceExt(ctx.frame_ctx))
        {
            pc.slot_count       = ext->view_slot_capacity != 0u
                ? ext->view_slot_capacity
                : ext->slot_count;
            pc.active_mask_addr = ext->active_mask_addr;
            pc.graph_material_addr = ext->graph_material_addr;
            pc.wanted_mip_addr = ext->wanted_mip_addr;
            pc.graph_material_capacity = ext->graph_material_capacity;
            pc.texture_slot_capacity = ext->texture_slot_capacity;
        }
        else
        {
            // makeViewCullPushConstants 本就把这两个字段种成 0,此处与其说是纠正
            // 不如说是把"ext 缺席 ⇒ 不剔除任何东西、也不读掩码"这条不变量写死在
            // 消费端,免得日后哪个 emit 路径烘了非零值就静默走样。
            pc.slot_count       = 0;
            pc.active_mask_addr = 0ull;
            pc.graph_material_addr = 0ull;
            pc.wanted_mip_addr = 0ull;
            pc.graph_material_capacity = 0u;
            pc.texture_slot_capacity = 0u;
        }

        if (pc.slot_count > 0 && ctx.current_layout != VK_NULL_HANDLE)
        {
            vkCmdPushConstants(ctx.cmd, ctx.current_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(pc), &pc);
        }
    }

    // =========================================================================
    //  MeshDraw — emit
    // =========================================================================

    static void emitMeshDrawKernel(ProgramEmitter& e, uint32_t pi,
                                   const RGCompiledPass& cpass,
                                   const RGCompiledGraph& compiled)
    {
        if (!compiled.mesh_bucket_layout.has_value()
            || cpass.pass->kernel_config.size < sizeof(MeshDrawKernelConfig))
            return;

        const auto& cfg = cpass.pass->kernel_config.as<MeshDrawKernelConfig>();

        // 索引缓冲的绑定改走**本 kernel 自己的子命令**,不再借用 L2 的领域定型
        // opcode BindMeshBuffers ——后者让渲染图录制器去全局注册表里 find<
        // MeshResources>(),是 L2 反向认识 L3 的抽象泄露。现在特性把这条缓冲
        // importBuffer 进图,kernel 只拿到一个通用资源下标,录制期经 resolveBuffer
        // 解析。顺带删掉了原先那次 vkCmdBindVertexBuffers:本路径的管线一律
        // clear() 掉顶点输入(顶点数据走 set 7 存储缓冲读),绑了无人消费。
        //
        // 句柄为空 = 场景里根本没有网格存储,此时 lanes 也必为空;直接返回,
        // 避免录进一串没有索引缓冲可用的 indexed indirect draw。
        if (cfg.index_buffers_rg == nullptr || cfg.index_buffer_count == 0u)
            return;

        const KernelTypeId kid = cpass.pass->kernel_id;
        std::uint16_t last_ibo_segment = ~std::uint16_t{0u};
        VkIndexType last_index_type = VK_INDEX_TYPE_MAX_ENUM;

        for (const auto& lane : compiled.mesh_bucket_layout->lanes)
        {
            if (lane.pass_index != pi) continue;
            if (lane.ibo_segment >= cfg.index_buffer_count)
                continue;

            if (lane.bind_pipeline)
            {
                struct { VkPipeline pipeline; VkPipelineLayout layout; }
                    bp{lane.pipeline, cpass.render.pipeline_layout};
                e.emit(ExecutionProgram::Command::EType::BindPipeline,
                       &bp, static_cast<uint16_t>(sizeof(bp)));
            }

            if (lane.ibo_segment != last_ibo_segment ||
                lane.index_type != last_index_type)
            {
                struct
                {
                    uint32_t resource_idx;
                    uint32_t index_type;
                } bind_idx{
                    cfg.index_buffers_rg[lane.ibo_segment].index,
                    static_cast<uint32_t>(lane.index_type)};
                e.emitKernelCommand(
                    kid,
                    kMeshDrawBindIndexBuffer,
                    &bind_idx,
                    static_cast<uint16_t>(sizeof(bind_idx)));
                last_ibo_segment = lane.ibo_segment;
                last_index_type = lane.index_type;
            }

            struct {
                uint32_t     indirect_resource_idx;
                uint32_t     count_resource_idx;
                VkDeviceSize indirect_offset;
                VkDeviceSize count_offset;
                uint32_t     max_draws;
                uint32_t     stride;
            } draw{cfg.indirect_rg.index, cfg.draw_count_rg.index,
                   lane.indirect_offset, lane.count_offset,
                   1u, kIndirectCommandSize};
            e.emit(ExecutionProgram::Command::EType::DrawIndexedIndirectCount,
                   &draw, static_cast<uint16_t>(sizeof(draw)));
        }
    }

    // =========================================================================
    //  MeshDraw — replay
    // =========================================================================

    /// 录制期绑定共享网格索引缓冲。资源下标经通用的 resolveBuffer 解析,
    /// 录制器因此无需认识 MeshResources。
    static void replayMeshDrawCommand(uint32_t sub_cmd, const void* data,
                                      uint16_t data_size, KernelReplayContext& ctx)
    {
        if (sub_cmd != kMeshDrawBindIndexBuffer)
            return;

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
    }

    // =========================================================================
    //  MeshDraw — mesh layout contribution
    // =========================================================================

    static void meshDrawContributeMesh(uint32_t pi,
                                       const RGCompiledPass& cpass,
                                       MeshBucketLayoutPlan& plan,
                                       PipelineManager& /*pipeline_manager*/)
    {
        if (cpass.pass->kernel_config.size < sizeof(MeshDrawKernelConfig))
            return;

        const auto& cfg = cpass.pass->kernel_config.as<MeshDrawKernelConfig>();
        const uint32_t mdc_count   = cfg.mdc_count;
        const MdcEntry* mdc_entries = cfg.mdc_entries;

        if (mdc_count == 0 || mdc_entries == nullptr)
            return;

        const auto& variants = cpass.render.pipeline_variants;
        // When a feature registers skinned pipeline variants, they live at
        // [family_count, 2*family_count) in `variants`. family_count == 0
        // (any pass without skinned variants) makes vidx == bucket_id
        // exactly — a strict no-op.
        const uint32_t family_count = cfg.family_count;
        for (uint32_t m = 0; m < mdc_count; ++m)
        {
            const auto& entry = mdc_entries[m];
            const uint32_t bucket = entry.bucket_id;
            uint32_t vidx = bucket;
            if (family_count > 0u
                && entry.geometry_kind == static_cast<uint8_t>(EGeometryKind::SkinnedMesh)
                && (bucket + family_count) < variants.size())
            {
                vidx = bucket + family_count;  // select the skinned variant
            }
            VkPipeline pipe = (vidx < variants.size()) ? variants[vidx] : VK_NULL_HANDLE;
            if (pipe == VK_NULL_HANDLE)
                continue;

            MeshLane lane{};
            lane.lane_id       = static_cast<uint32_t>(plan.lanes.size());
            lane.pass_index    = pi;
            lane.geometry_kind = static_cast<uint8_t>(entry.geometry_kind);
            lane.bucket_id     = bucket;
            lane.mdc_index     = m;
            lane.ibo_segment   = entry.ibo_segment;
            lane.index_type    = entry.index_type;
            lane.pipeline      = pipe;
            plan.lanes.push_back(lane);
        }
    }

    // =========================================================================
    //  MeshCull — DynamicPatch resolution (KernelDescriptor::PatchFn)
    // =========================================================================

    static uint32_t resolveMeshPatch(uint16_t source_param,
                                      const RGFrameContext& frame_ctx)
    {
        switch (source_param) {
        case kSlotCountPatch:
        {
            const auto* ext = getInstanceExt(frame_ctx);
            const uint32_t count = ext
                ? (ext->view_slot_capacity != 0u
                    ? ext->view_slot_capacity
                    : ext->slot_count)
                : 0u;
            return (count + kCullDispatchWorkgroupSize - 1u) /
                kCullDispatchWorkgroupSize;
        }
        default:
            return 0;
        }
    }

} // namespace lux::render::kernels

// =============================================================================
//  Self-registration
// =============================================================================

LUX_REGISTER_KERNEL("MeshCull",
    (lux::render::KernelDescriptor{
        .emit             = &lux::render::kernels::emitMeshCullKernel,
        .contribute_mesh  = nullptr,
        .contribute_arena = &lux::render::kernels::meshCullContributeArena,
        // 剔除推送常量改由本 kernel 回放,替代已删除的 L2 opcode
        // CullPushConstants(见 emitMeshCullKernel 处说明)。
        .replay           = &lux::render::kernels::replayMeshCullCommand,
        .resolve_patch    = &lux::render::kernels::resolveMeshPatch,
        .ext_slot_name    = "mesh_instance",
    }))

LUX_REGISTER_KERNEL("MeshDraw",
    (lux::render::KernelDescriptor{
        .emit             = &lux::render::kernels::emitMeshDrawKernel,
        .contribute_mesh  = &lux::render::kernels::meshDrawContributeMesh,
        .contribute_arena = nullptr,
        // 索引缓冲绑定改由本 kernel 回放,替代已删除的 L2 领域 opcode
        // BindMeshBuffers(见 emitMeshDrawKernel 处说明)。
        .replay           = &lux::render::kernels::replayMeshDrawCommand,
    }))

namespace lux::render
{
    FrameExtensionSlotId meshInstanceExtSlot() noexcept
    {
        static const FrameExtensionSlotId slot =
            KernelRegistry::instance().extSlotOf("mesh_instance");
        return slot;
    }
} // namespace lux::render
