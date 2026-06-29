// ============================================================================
//  render_graph_qfot_guard_test.cpp
//
//  Pins the M4 single-queue / QFOT fail-fast contract in RenderGraphCompiler.
//
//  Contract: the render graph does NOT implement queue-family ownership
//  transfer (QFOT). Every RG resource is created VK_SHARING_MODE_EXCLUSIVE, and
//  the recorder emits every barrier with VK_QUEUE_FAMILY_IGNORED. A graph that
//  makes one queue WRITE a resource and a DIFFERENT queue READ it would need a
//  QFOT to be correct; since QFOT is unimplemented, compile() must FAIL FAST
//  (compiled.valid == false, with a compile_error that names the cross-queue
//  ownership transfer) instead of silently producing a graph whose consumer
//  reads undefined contents.
//
//  We drive the REAL public compile() entry point. To reach the cross-queue
//  analysis we use transfer-family passes (a regular TRANSFER producer on the
//  GRAPHICS queue + an ASYNC_TRANSFER consumer on the TRANSFER queue): they need
//  no pipeline and no color/depth attachment (a buffer-only GRAPHICS pass is
//  rejected earlier by the render-pass planner), and the M4 guard fires before
//  any device-dependent compile stage runs.
//
//  A minimal headless device is brought up ONLY to satisfy compile()'s
//  PipelineManager& parameter (never actually used once the guard trips). The
//  test skips (exit 0) when no Vulkan device is present, matching the other
//  device-dependent render tests (descriptor_arena_growth, thumbnail, ...).
//
//  Self-checking: 0 = PASS (or skip if no Vulkan device), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGResourceTypes.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace lux::render;

static int g_fail = 0;
static void check(bool cond, const char* name)
{
    std::printf(cond ? "  [PASS] %s\n" : "  [FAIL] %s\n", name);
    if (!cond) ++g_fail;
}

// A minimal IMPORTED buffer. IMPORTED lifetime pins the resource as "exported",
// so dead-pass elimination keeps any producer/consumer pass that touches it.
// The getter is never invoked at compile time but must be non-null to pass the
// compiler's imported-resource validation.
static RGResourceHandle addPinnedBuffer(RGBuilder& b, const char* name)
{
    RGBufferDescription desc{};
    desc.size          = 256;
    desc.stride        = 4;
    desc.element_count = 64;
    desc.usage         = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::NONE);
    desc.usage        |= ERGBufferUsageBits::STORAGE;
    desc.memory_usage  = ERGMemoryUsage::GPU_ONLY;

    RGImportedBufferInfo imp{};
    imp.buffer_getter = [](VkBuffer*, uint32_t) -> uint32_t { return 0; };

    return b.importBuffer(name, desc, imp);
}

// Two passes sharing buffer "SharedBuf": a regular TRANSFER writer (assigned to
// the GRAPHICS queue by computeQueueAssignment) and a reader on `consumer_type`'s
// queue. Both are transfer-family passes so they need neither a pipeline nor a
// color/depth attachment (a buffer-only GRAPHICS pass is rejected earlier by the
// render-pass planner). Each pass also writes its own pinned output so neither is
// pruned by dead-pass elimination before the cross-queue walk.
//
//   consumer_type == ASYNC_TRANSFER -> reader on the TRANSFER queue  => cross-queue
//   consumer_type == TRANSFER       -> reader on the GRAPHICS queue  => single-queue
static RGGraphDescription buildSharedBufferGraph(ERGPassType consumer_type)
{
    RGBuilder b;
    auto shared = addPinnedBuffer(b, "SharedBuf");
    auto out0   = addPinnedBuffer(b, "WriterOut");
    auto out1   = addPinnedBuffer(b, "ReaderOut");

    b.addPass("Writer", ERGPassType::TRANSFER)
        .write(shared, ERGBufferRole::STORAGE)
        .write(out0,   ERGBufferRole::STORAGE);

    b.addPass("Reader", consumer_type)
        .read(shared,  ERGBufferRole::STORAGE)
        .write(out1,   ERGBufferRole::STORAGE);

    return std::move(b).build();
}

static bool tripsQfotGuard(const RGCompiledGraph& compiled)
{
    return compiled.compile_error.find("cross-queue") != std::string::npos
        || compiled.compile_error.find("QFOT")        != std::string::npos;
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== render_graph_qfot_guard_test ===\n");

    // Headless device — only needed to construct the PipelineManager& that
    // compile() takes by reference. Skip gracefully if Vulkan is unavailable.
    std::unique_ptr<InstanceContext> inst;
    try {
        inst = std::make_unique<InstanceContext>(std::vector<const char*>{}, nullptr);
    } catch (...) {
        std::printf("No Vulkan instance. Skipping.\n");
        return 0;
    }

    DeviceContext dev{*inst};
    if (auto r = dev.init(EPhysicalDeviceSelectionPolicy::INTEGRATED_GPU_PREFERRED); !r) {
        std::printf("No Vulkan device. Skipping.\n");
        return 0;
    }
    PipelineManager pm{dev, false};

    // ── Positive: cross-queue (GRAPHICS-queue write -> TRANSFER-queue read) ─
    {
        auto compiled = RenderGraphCompiler::compile(
            buildSharedBufferGraph(ERGPassType::ASYNC_TRANSFER), pm);

        check(!compiled.valid,
              "cross-queue EXCLUSIVE sharing is rejected (compiled.valid == false)");
        check(tripsQfotGuard(compiled),
              "compile_error explains the cross-queue ownership-transfer rejection");
        std::printf("    compile_error: %s\n", compiled.compile_error.c_str());
    }

    // ── Negative: same structure, single queue (both on GRAPHICS queue) ────
    {
        auto compiled = RenderGraphCompiler::compile(
            buildSharedBufferGraph(ERGPassType::TRANSFER), pm);

        check(!tripsQfotGuard(compiled),
              "single-queue buffer sharing does NOT trip the QFOT guard");
        check(compiled.valid,
              "single-queue graph compiles cleanly (compiled.valid == true)");
        if (!compiled.valid)
            std::printf("    compile_error: %s\n", compiled.compile_error.c_str());
    }

    std::printf("=== render_graph_qfot_guard_test %s (fails=%d) ===\n",
                g_fail == 0 ? "PASSED" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
