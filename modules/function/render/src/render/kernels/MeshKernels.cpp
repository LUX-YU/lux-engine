/**
 * @file MeshKernels.cpp
 * @brief Kernel descriptors for GPU-driven mesh geometry paths.
 *
 * Registers:
 *   EPassKernel::MeshCull  — frustum cull + dispatch
 *   EPassKernel::MeshDraw  — indirect draw lanes + mesh buffer bind
 *
 * Contribution hooks:
 *   MeshCull::contribute_arena — counts frustum UBO slots
 *   MeshDraw::contribute_mesh  — generates ordered MeshLane entries
 */

#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/graph/ProgramEmitter.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/resources/mesh/MdcTable.hpp>
#include <lux/engine/render/core/RenderObjectTypes.hpp>   // EGeometryKind
#include <lux/engine/render/graph/GpuDrivenMeshConsts.hpp>
#include <lux/engine/render/graph/MeshInstanceExtData.hpp>

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
    //  Constants (shared with recorder)
    // =========================================================================

    static constexpr uint32_t kIndirectCommandSize = 20; // sizeof(VkDrawIndexedIndirectCommand)
    static constexpr uint32_t kWorkgroupSize       = 64;

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

        uint32_t frustum_res = cfg.frustum_ubo_rg.index;
        e.emit(ExecutionProgram::Command::EType::UploadViewFrustum,
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
        auto pc = makeViewCullPushConstants(cfg.pass_mask, cfg.geometry_mask,
                                            cfg.extension_flags);
        e.emit(ExecutionProgram::Command::EType::CullPushConstants,
               &pc, static_cast<uint16_t>(sizeof(pc)));

        struct { uint32_t x, y, z; } dispatch{1, 1, 1};
        const uint32_t cmd_idx = e.emit(
            ExecutionProgram::Command::EType::Dispatch,
            &dispatch, static_cast<uint16_t>(sizeof(dispatch)));

        const KernelTypeId kid = cpass.pass->kernel_id;
        ExecutionProgram::DynamicPatch patch{};
        patch.command_index     = cmd_idx;
        patch.data_field_offset = 0;
        patch.source            = ExecutionProgram::DynamicPatch::ESource::KernelPatch;
        patch.source_param      = (static_cast<uint16_t>(kid) << 8) | kSlotCountPatch;
        e.program.patches.push_back(patch);
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
        e.emit(ExecutionProgram::Command::EType::BindMeshBuffers, nullptr, 0);

        for (const auto& lane : compiled.mesh_bucket_layout->lanes)
        {
            if (lane.pass_index != pi) continue;

            if (lane.bind_pipeline)
            {
                struct { VkPipeline pipeline; VkPipelineLayout layout; }
                    bp{lane.pipeline, cpass.render.pipeline_layout};
                e.emit(ExecutionProgram::Command::EType::BindPipeline,
                       &bp, static_cast<uint16_t>(sizeof(bp)));
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
            const uint32_t alive = ext ? ext->slot_count : 0;
            return (alive + 63u) / 64u;
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
        .replay           = nullptr,
        .resolve_patch    = &lux::render::kernels::resolveMeshPatch,
        .ext_slot_name    = "mesh_instance",
    }))

LUX_REGISTER_KERNEL("MeshDraw",
    (lux::render::KernelDescriptor{
        .emit             = &lux::render::kernels::emitMeshDrawKernel,
        .contribute_mesh  = &lux::render::kernels::meshDrawContributeMesh,
        .contribute_arena = nullptr,
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
