// ============================================================================
//  exportable_memory_test.cpp
//
//  Exercises the domain-neutral CUDA<->Vulkan external-memory SEAM added to the
//  render module (RenderContextView::createExportableBuffer /
//  createExportableTimelineSemaphore + the DeferredDestroyQueue raw-handle retire
//  path). NO CUDA, NO Gaussian/3DGS semantics, NO downstream code — the engine
//  only proves it can hand out an EXPORTABLE Vulkan buffer + two exportable
//  timeline semaphores, and retire them safely.
//
//  A neutral ExportProbeFeature runs in initAndAttachTo (render-thread, context
//  valid): it allocates one exportable buffer + two exportable timeline
//  semaphores via the public contextView() facade and records the results into
//  static atomics (same observe-via-static-counter trick as the lifecycle test's
//  CountingFeature — no query op needed). onDetachFromScene retires the handles,
//  exercising the new raw-handle deferred-destroy cases.
//
//  Reuses readback_test's window-attached server+session harness. Self-checking:
//  0 = PASS / skip (no Vulkan, or device lacks external-memory support — the
//  NVIDIA box supports it, so it runs the assertions), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>     // RenderScene::addFeature<T> in create_fn
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/FeatureDescriptor.hpp>

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace lux::render;

namespace
{
    // ── Test-only feature: allocates exportable interop resources, records flags ──
    struct ExportProbeFeature : RenderFeature
    {
        static std::atomic<int>  ran;             // initAndAttachTo executed
        static std::atomic<bool> supports;        // supportsExternalMemory()
        static std::atomic<bool> uuid_nonzero;    // deviceUUID() not all-zero
        static std::atomic<bool> buffer_ok;       // buffer + memory non-null, handle != 0, size >= requested
        static std::atomic<bool> sem1_ok;         // semaphore non-null + handle != 0
        static std::atomic<bool> sem2_ok;
        static std::atomic<int>  detached;        // onDetachFromScene executed (retire ran)

        static constexpr uint64_t kBufferSize = 256;

        // Stored so onDetachFromScene can retire them (FIF-safe deferred destroy).
        ExportableBuffer            buf_{};
        ExportableTimelineSemaphore produce_done_{};
        ExportableTimelineSemaphore consume_done_{};

        ExportProbeFeature() : RenderFeature(RenderFeature::Config{"ExportProbe"}) {}

        void initAndAttachTo(RenderScene& /*scene*/) override
        {
            auto cv = contextView();

            const bool sup = cv.supportsExternalMemory();
            supports.store(sup, std::memory_order_relaxed);

            std::uint8_t uuid[16] = {};
            cv.deviceUUID(uuid);
            bool any = false;
            for (auto b : uuid) any = any || (b != 0);
            uuid_nonzero.store(any, std::memory_order_relaxed);

            if (sup)
            {
                buf_          = cv.createExportableBuffer(kBufferSize, /*usage=*/0);
                produce_done_ = cv.createExportableTimelineSemaphore();
                consume_done_ = cv.createExportableTimelineSemaphore();

                buffer_ok.store(buf_.buffer != VkBuffer{} && buf_.memory != VkDeviceMemory{} &&
                                buf_.external_handle != 0 && buf_.actual_size >= kBufferSize,
                                std::memory_order_relaxed);
                sem1_ok.store(produce_done_.semaphore != VkSemaphore{} && produce_done_.external_handle != 0,
                              std::memory_order_relaxed);
                sem2_ok.store(consume_done_.semaphore != VkSemaphore{} && consume_done_.external_handle != 0,
                              std::memory_order_relaxed);
            }

            ran.fetch_add(1, std::memory_order_relaxed);
        }

        void onDetachFromScene(RenderScene& /*scene*/) override
        {
            auto cv = contextView();
            // Retire whatever was allocated. Null handles are ignored by the queue.
            if (buf_.buffer != VkBuffer{})
                cv.retireBuffer(buf_.buffer, nullptr);   // dedicated (non-VMA): allocation == null
            if (buf_.memory != VkDeviceMemory{})
                cv.retireDeviceMemory(buf_.memory);
            if (produce_done_.semaphore != VkSemaphore{})
                cv.retireSemaphore(produce_done_.semaphore);
            if (consume_done_.semaphore != VkSemaphore{})
                cv.retireSemaphore(consume_done_.semaphore);
            detached.fetch_add(1, std::memory_order_relaxed);
        }

        void addPasses(RGBuilder& /*builder*/) override {}
    };
    std::atomic<int>  ExportProbeFeature::ran{0};
    std::atomic<bool> ExportProbeFeature::supports{false};
    std::atomic<bool> ExportProbeFeature::uuid_nonzero{false};
    std::atomic<bool> ExportProbeFeature::buffer_ok{false};
    std::atomic<bool> ExportProbeFeature::sem1_ok{false};
    std::atomic<bool> ExportProbeFeature::sem2_ok{false};
    std::atomic<int>  ExportProbeFeature::detached{0};

    FeatureHandle probeCreateFn(void* scene, const void*, std::size_t)
    {
        return static_cast<RenderScene*>(scene)->addFeature<ExportProbeFeature>();
    }

    constexpr FeatureDescriptor kProbeDesc{
        .type = featureId("lux.test.export_probe.v1"),
        .name = "ExportProbe",
    };

    struct EmptyConfig {};

    FeatureFactory kProbeFactory() { return makeSimpleFactory(&probeCreateFn, "ExportProbe", kProbeDesc); }

    std::vector<const char*> getVulkanExtensions()
    {
        glfwInit();
        std::uint32_t count = 0;
        const char** exts = glfwGetRequiredInstanceExtensions(&count);
        std::vector<const char*> result;
        for (std::uint32_t i = 0; i < count; ++i) result.emplace_back(exts[i]);
        return result;
    }

    template <class T>
    T await(RenderSession& s, lux::window::LuxWindow& w, RenderRequest<T> req)
    {
        s.submitFrame(/*blocking=*/true);
        while (!req.isReady()) { w.pollEvents(); s.waitAndPumpReplies(); }
        T r = req.result();
        s.beginFrame({});
        return r;
    }

    void flush(RenderSession& s, lux::window::LuxWindow& w)
    {
        s.submitFrame(/*blocking=*/true);
        w.pollEvents();
        s.waitAndPumpReplies();
        s.beginFrame({});
    }
}

int main()
{
    std::cout << std::unitbuf;
    std::cout << "=== exportable_memory_test ===\n";

    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(64, 64, "exportable_memory_test");

    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::thread server_thread([&]
    {
        GeneralRenderServer server(channel, sync);
        ServerConfig cfg;
        cfg.instance_extensions = exts;
        if (auto r = server.init(std::move(cfg)); !r)
        { failed.store(true); ready.store(true); return; }
        if (auto r = server.attachToWindow(window); !r)
        { failed.store(true); ready.store(true); return; }
        ready.store(true);
        try { while (server.tick()) {} } catch (...) {}
    });

    while (!ready.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (failed.load(std::memory_order_acquire))
    {
        std::cerr << "Server init failed (no Vulkan device?). Skipping.\n";
        sync->requestStop(); server_thread.join();
        return 0; // skip
    }

    RenderSession session(channel, sync);
    session.beginFrame({});

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n"; if (!cond) ++fails; };

    const auto scene = await(session, window, session.createScene("Export"));
    await(session, window, session.setActiveScene(scene.scene_id, true));

    const auto probe_type = await(session, window,
        session.registerFeatureType(kProbeFactory()));

    const auto pf = await(session, window,
        session.addFeature(scene.scene_id, probe_type.feature_type_id, EmptyConfig{}));
    check(pf.feature.valid(), "addFeature(ExportProbe) succeeded");
    flush(session, window);

    check(ExportProbeFeature::ran.load() == 1, "probe initAndAttachTo ran");

    if (!ExportProbeFeature::supports.load())
    {
        std::cout << "  [SKIP] device reports no external-memory support; skipping interop asserts.\n";
        sync->requestStop();
        server_thread.join();
        std::cout << "=== exportable_memory_test PASSED (skipped) ===\n";
        return 0;
    }

    check(ExportProbeFeature::uuid_nonzero.load(), "deviceUUID() is non-zero");
    check(ExportProbeFeature::buffer_ok.load(),
          "createExportableBuffer yields buffer+memory+handle, size >= requested");
    check(ExportProbeFeature::sem1_ok.load(), "createExportableTimelineSemaphore #1 yields semaphore + handle");
    check(ExportProbeFeature::sem2_ok.load(), "createExportableTimelineSemaphore #2 yields semaphore + handle");

    // removeFeature → onDetachFromScene retires the raw interop handles
    // (exercises the new DeferredDestroyQueue DeviceMemory/Semaphore cases).
    session.removeFeature(scene.scene_id, pf.feature);
    flush(session, window);
    check(ExportProbeFeature::detached.load() == 1, "onDetachFromScene retired the interop handles");

    sync->requestStop();
    server_thread.join();

    std::cout << "=== exportable_memory_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
