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
#include <lux/engine/render/graph/ProgramEmitter.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>

#include <algorithm>
#include <cstring>

namespace lux::render::kernels
{
    static constexpr uint32_t kWorkgroupSize = 64;

    // =========================================================================
    //  MdcCompact — emit
    // =========================================================================

    static void emitMdcCompactKernel(ProgramEmitter& e, uint32_t /*pi*/,
                                     const RGCompiledPass& cpass,
                                     const RGCompiledGraph& /*compiled*/)
    {
        if (cpass.pass->kernel_config.size < sizeof(MdcCompactKernelConfig))
            return;

        const auto& cfg = cpass.pass->kernel_config.as<MdcCompactKernelConfig>();

        struct { uint32_t mdc_count; } pc{cfg.mdc_count};
        e.emit(ExecutionProgram::Command::EType::CullPushConstants,
               &pc, static_cast<uint16_t>(sizeof(pc)));

        struct { uint32_t x, y, z; } dispatch{
            (cfg.mdc_count + kWorkgroupSize - 1) / kWorkgroupSize, 1, 1};
        e.emit(ExecutionProgram::Command::EType::Dispatch,
               &dispatch, static_cast<uint16_t>(sizeof(dispatch)));
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
            clear.dispatch_x = (max_elements + kWorkgroupSize - 1) / kWorkgroupSize;
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
        .emit = &lux::render::kernels::emitMdcCompactKernel,
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
