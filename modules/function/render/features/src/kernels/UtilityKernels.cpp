/**
 * @file UtilityKernels.cpp
 * @brief Kernel descriptors for utility and trivial-draw passes.
 *
 * Registers:
 *   EPassKernel::MdcCompact      — MDC compaction compute dispatch
 *   EPassKernel::ClearCounters   — multi-buffer counter clear dispatch
 *   EPassKernel::FullscreenQuad  — fullscreen triangle draw (vkCmdDraw 3,1,0,0)
 *   EPassKernel::SkyboxDraw      — fullscreen triangle (cube-map skybox)
 *   EPassKernel::GridDraw        — fullscreen triangle (infinite grid)
 *   EPassKernel::TonemapPass     — fullscreen triangle (tonemap)
 *   EPassKernel::DeferredLighting — fullscreen triangle (deferred shading)
 *   EPassKernel::DepthPrepass    — fullscreen triangle (depth-only)
 */

#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/graph/KernelReplayContext.hpp>
#include <lux/engine/render/graph/ProgramEmitter.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>

#include <algorithm>
#include <cstring>

namespace lux::render::kernels
{

    // =========================================================================
    //  MdcCompact — emit
    // =========================================================================

    /// MdcCompact 的 kernel 局部子命令 id。
    enum MdcCompactSubCmd : uint8_t
    {
        kMdcCompactPushConstants = 0,
    };

    static void emitMdcCompactKernel(ProgramEmitter& e, uint32_t /*pi*/,
                                     const RGCompiledPass& cpass,
                                     const RGCompiledGraph& /*compiled*/)
    {
        if (cpass.pass->kernel_config.size < sizeof(MdcCompactKernelConfig))
            return;

        const auto& cfg = cpass.pass->kernel_config.as<MdcCompactKernelConfig>();

        // 推送常量走**本 kernel 自己的子命令**,不再借用通用 opcode
        // EType::CullPushConstants。
        //
        // 原先两者共用该 opcode,但载荷根本不兼容:MeshCull 发 48 字节的
        // MeshCullPushConstants,本 kernel 发 4 字节的 {mdc_count}。而录制器对该
        // opcode 是**无条件**按 MeshCullPushConstants 解释的 —— 它把这 4 字节拷进
        // pc.slot_count,紧接着又用 ext->slot_count 覆盖掉(mdc_count 就此丢失),
        // 然后推 48 字节,而 mdc_compact.comp 只声明 4 字节 PC 块。
        //
        // 这条路径今天走不到,只因为两处 MdcCompact pass 都挂了 setKernelFn,被
        // classifyExecutionModes 判为 COMPILED_CALLBACK 而绕过 kernel body ——
        // 也就是说 bug 是"已装填未击发":谁哪天去掉那个 kernel_fn,它就会静默生效。
        // 根因是那个 opcode 在 L2 是**领域定型**的,共享它的 kernel 无法有不同载荷。
        //
        // 该 opcode 现已从 L2 整条删除(最后一个使用者 MeshCull 也迁到了自己的
        // 子命令),所以上面描述的地雷已不复存在 —— 此段保留为设计教训:
        // **别在 L2 的通用指令集里放领域定型的载荷**。
        const KernelTypeId kid = cpass.pass->kernel_id;
        struct { uint32_t mdc_count; } pc{cfg.mdc_count};
        e.emitKernelCommand(kid, kMdcCompactPushConstants,
                            &pc, static_cast<uint16_t>(sizeof(pc)));

        struct { uint32_t x, y, z; } dispatch{
            (cfg.mdc_count + kCullDispatchWorkgroupSize - 1) / kCullDispatchWorkgroupSize, 1, 1};
        e.emit(ExecutionProgram::Command::EType::Dispatch,
               &dispatch, static_cast<uint16_t>(sizeof(dispatch)));
    }

    /// 录制期回放本 kernel 的子命令。按 mdc_compact.comp 的实际 PC 块(4 字节)
    /// 推送,不再被当成 48 字节的网格剔除 PC。
    static void replayMdcCompactCommand(uint32_t sub_cmd, const void* data,
                                        uint16_t data_size, KernelReplayContext& ctx)
    {
        if (sub_cmd != kMdcCompactPushConstants)
            return;

        struct { uint32_t mdc_count; } pc{};
        if (data_size < sizeof(pc))
            return;
        std::memcpy(&pc, data, sizeof(pc));

        if (pc.mdc_count > 0 && ctx.current_layout != VK_NULL_HANDLE)
        {
            vkCmdPushConstants(ctx.cmd, ctx.current_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(pc), &pc);
        }
    }

    // =========================================================================
    //  ClearCounters — emit
    // =========================================================================

    static void emitClearCountersKernel(ProgramEmitter& e, uint32_t /*pi*/,
                                        const RGCompiledPass& cpass,
                                        const RGCompiledGraph& /*compiled*/)
    {
        if (cpass.pass->kernel_config.size < sizeof(ClearCountersKernelConfig))
            return;

        const auto& cfg = cpass.pass->kernel_config.as<ClearCountersKernelConfig>();
        const uint32_t buffer_count = std::min(
            cfg.buffer_count, ClearCountersKernelConfig::kMaxBuffers);

        struct {
            uint32_t resource_indices[ClearCountersKernelConfig::kMaxBuffers];
            uint32_t element_counts[ClearCountersKernelConfig::kMaxBuffers];
            uint32_t buffer_count;
            uint32_t dispatch_x;
        } clear{};

        uint32_t max_elements = 0;
        for (uint32_t bi = 0; bi < buffer_count; ++bi)
        {
            const RGResourceHandle rg = cfg.buffers[bi];
            if (!rg) continue;

            const VkDeviceSize clear_size = e.resolveBufferSizeBytes(rg);
            if (clear_size < sizeof(uint32_t)) continue;

            const uint32_t element_count =
                static_cast<uint32_t>(clear_size / sizeof(uint32_t));
            if (element_count == 0) continue;

            clear.resource_indices[clear.buffer_count] = rg.index;
            clear.element_counts[clear.buffer_count]   = element_count;
            ++clear.buffer_count;
            max_elements = std::max(max_elements, element_count);
        }

        if (clear.buffer_count > 0 && max_elements > 0)
        {
            clear.dispatch_x = (max_elements + kCullDispatchWorkgroupSize - 1) / kCullDispatchWorkgroupSize;
            e.emit(ExecutionProgram::Command::EType::ClearCounters,
                   &clear, static_cast<uint16_t>(sizeof(clear)));
        }
    }

    // =========================================================================
    //  SimpleDraw — shared emit for all fullscreen-triangle passes
    // =========================================================================

    static void emitSimpleDrawKernel(ProgramEmitter& e, uint32_t /*pi*/,
                                     const RGCompiledPass& /*cpass*/,
                                     const RGCompiledGraph& /*compiled*/)
    {
        // Fullscreen triangle: vkCmdDraw(3, 1, 0, 0)
        struct { uint32_t vtx_count, inst_count, first_vtx, first_inst; }
            draw{3, 1, 0, 0};
        e.emit(ExecutionProgram::Command::EType::DrawDirect,
               &draw, static_cast<uint16_t>(sizeof(draw)));
    }

} // namespace lux::render::kernels

// =============================================================================
//  Self-registration
// =============================================================================

LUX_REGISTER_KERNEL("MdcCompact",
    (lux::render::KernelDescriptor{
        .emit   = &lux::render::kernels::emitMdcCompactKernel,
        // 推送常量走本 kernel 自己的子命令回放。原先借用的那条 L2 领域定型 opcode
        // 载荷不兼容,现已整条删除(见 emitMdcCompactKernel 处说明)。
        .replay = &lux::render::kernels::replayMdcCompactCommand,
    }))

LUX_REGISTER_KERNEL("ClearCounters",
    (lux::render::KernelDescriptor{
        .emit = &lux::render::kernels::emitClearCountersKernel,
    }))

// Fullscreen-triangle passes — all share emitSimpleDrawKernel
LUX_REGISTER_KERNEL("FullscreenQuad",
    (lux::render::KernelDescriptor{ .emit = &lux::render::kernels::emitSimpleDrawKernel }))
LUX_REGISTER_KERNEL("SkyboxDraw",
    (lux::render::KernelDescriptor{ .emit = &lux::render::kernels::emitSimpleDrawKernel }))
LUX_REGISTER_KERNEL("GridDraw",
    (lux::render::KernelDescriptor{ .emit = &lux::render::kernels::emitSimpleDrawKernel }))
LUX_REGISTER_KERNEL("TonemapPass",
    (lux::render::KernelDescriptor{ .emit = &lux::render::kernels::emitSimpleDrawKernel }))
LUX_REGISTER_KERNEL("DeferredLighting",
    (lux::render::KernelDescriptor{ .emit = &lux::render::kernels::emitSimpleDrawKernel }))
LUX_REGISTER_KERNEL("DepthPrepass",
    (lux::render::KernelDescriptor{ .emit = &lux::render::kernels::emitSimpleDrawKernel }))
