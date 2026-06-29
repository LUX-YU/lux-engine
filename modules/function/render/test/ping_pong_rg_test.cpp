// ============================================================================
//  ping_pong_rg_test.cpp  —  RG演进 M2: ping-pong cross-feature subscription,
//  dependency contract (required/optional), and minimal-overhead inspection.
//
//  Drives the REAL public compile() path on a tiny graph that mirrors the HZB
//  shape: a PRODUCER writes a ping-pong resource's current() copy; a CONSUMER
//  (separate pass, by-name subscription) reads its previous() copy. Buffer
//  ping-pong + TRANSFER passes keep it pipeline-free (like the QFOT guard test),
//  so compile reaches dependency/barrier analysis on any headless device.
//
//  Checks:
//    1. registered producer + Required reference  -> compiles; dump shows the
//       ping-pong pair and NO producer->consumer barrier (different copies =
//       no intra-frame RAW = minimal overhead).
//    2. missing producer + Required               -> compile FAILS FAST with a
//       "Required reference" error (not a silent null view / VUID).
//    3. missing producer + Optional               -> compiles (consumer degrades).
//
//  Self-checking: 0 = PASS (or skip if no Vulkan device), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGResourceTypes.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/graph/RGDebugPrint.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <iostream>
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

// A pinned IMPORTED buffer: its "exported" lifetime keeps the pass that writes it
// alive through dead-pass elimination (so neither producer nor consumer is pruned
// for an unrelated reason).
static RGResourceHandle addPinnedBuffer(RGBuilder& b, const char* name)
{
    RGBufferDescription desc{};
    desc.size = 256; desc.stride = 4; desc.element_count = 64;
    desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
    desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
    RGImportedBufferInfo imp{};
    imp.buffer_getter = [](VkBuffer*, uint32_t) -> uint32_t { return 0; };
    return b.importBuffer(name, desc, imp);
}

// Producer writes ping-pong "PP" current(); consumer reads previous(). Both are
// TRANSFER passes (no pipeline). `register_producer=false` omits the producer to
// exercise the unresolved-reference contract.
static RGGraphDescription buildPingPongGraph(ERGReference ref, bool register_producer)
{
    RGBuilder b;
    auto wout = addPinnedBuffer(b, "WriterOut");
    auto rout = addPinnedBuffer(b, "ReaderOut");

    if (register_producer)
    {
        RGBufferDescription bd{};
        bd.size = 1024; bd.stride = 4; bd.element_count = 256;
        bd.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
        bd.memory_usage = ERGMemoryUsage::GPU_ONLY;
        RGRingResourceHandle pp = b.createPingPongBuffer("PP", bd, 2);

        b.addPass("Producer", ERGPassType::TRANSFER)
            .write(pp.current(), ERGBufferRole::STORAGE)
            .write(wout,         ERGBufferRole::STORAGE);
    }

    // Consumer subscribes by NAME (no knowledge of the producer's type/feature).
    RGRingResourceHandle sub = b.referencePingPong("PP", ref);
    b.addPass("Consumer", ERGPassType::TRANSFER)
        .read(sub.previous(), ERGBufferRole::STORAGE)
        .write(rout,          ERGBufferRole::STORAGE);

    return std::move(b).build();
}

// Count barriers across all compiled passes (image + buffer).
static std::size_t totalBarriers(const RGCompiledGraph& g)
{
    std::size_t n = 0;
    for (const auto& cp : g.compiled_passes)
        n += cp.sync.prebuilt_image_barriers.size() + cp.sync.prebuilt_buffer_barriers.size();
    return n;
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== ping_pong_rg_test ===\n");

    std::unique_ptr<InstanceContext> inst;
    try { inst = std::make_unique<InstanceContext>(std::vector<const char*>{}, nullptr); }
    catch (...) { std::printf("No Vulkan instance. Skipping.\n"); return 0; }

    DeviceContext dev{*inst};
    if (auto r = dev.init(EPhysicalDeviceSelectionPolicy::INTEGRATED_GPU_PREFERRED); !r) {
        std::printf("No Vulkan device. Skipping.\n");
        return 0;
    }
    PipelineManager pm{dev, false};

    // ── 1. registered producer + Required → compiles; inspect minimal overhead ──
    {
        auto compiled = RenderGraphCompiler::compile(buildPingPongGraph(ERGReference::Required, true), pm);
        check(compiled.valid, "registered producer + Required → compiles");
        if (compiled.valid)
        {
            // PP current + PP.prev = two distinct logical resources (the ping-pong pair).
            check(compiled.original_graph.resources.size() >= 4,
                  "ping-pong creates a cur + prev resource pair");
            // Minimal overhead: producer writes cur, consumer reads prev — DIFFERENT
            // physical copies, so there is NO producer->consumer intra-frame RAW
            // barrier. Each pass keeps only its own first-touch transition.
            std::printf("  total barriers = %zu\n", totalBarriers(compiled));
            std::printf("  ---- compiled graph dump (inspect overhead) ----\n");
            printCompiledGraph(compiled, std::cout);
        }
        else std::printf("    compile_error: %s\n", compiled.compile_error.c_str());
    }

    // ── 2. missing producer + Required → fail fast ──
    {
        auto compiled = RenderGraphCompiler::compile(buildPingPongGraph(ERGReference::Required, false), pm);
        const bool named = compiled.compile_error.find("Required reference") != std::string::npos;
        check(!compiled.valid && named,
              "missing producer + Required → compile FAILS FAST with a named error");
        std::printf("    compile_error: %s\n", compiled.compile_error.c_str());
    }

    // ── 3. missing producer + Optional → compiles (consumer degrades) ──
    {
        auto compiled = RenderGraphCompiler::compile(buildPingPongGraph(ERGReference::Optional, false), pm);
        check(compiled.valid,
              "missing producer + Optional → compiles (consumer degrades, no error)");
        if (!compiled.valid)
            std::printf("    compile_error: %s\n", compiled.compile_error.c_str());
    }

    std::printf("=== ping_pong_rg_test %s (fails=%d) ===\n",
                g_fail == 0 ? "PASSED" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
