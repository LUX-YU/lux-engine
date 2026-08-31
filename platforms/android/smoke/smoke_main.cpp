// ============================================================================
//  smoke_main.cpp — Android bring-up harness.
//
//  Purpose: turn "we cross-compile 21 .so for arm64" into "the engine actually
//  runs on the phone", one question at a time. Everything here is a GATE: each
//  step logs a verdict and the next step only runs if the previous one passed,
//  so a failure names its own layer instead of surfacing as a black screen six
//  layers later.
//
//  Deliberately NOT the runtime player. A player would light up every unknown
//  at once; this lights them one at a time, in dependency order:
//
//    [1] .so loads + android_main entered      — linker resolves at runtime
//    [2] ComponentTypeCatalog non-empty         — reflection sidecar alive
//    [3] Vulkan instance + device limits        — the REAL maxBoundDescriptorSets
//    [4] ANativeWindow -> VkSurfaceKHR          (next iteration)
//    [5] swapchain clear to a solid colour      (next iteration)
//    [6] attach the editor feature set          (next iteration)
//
//  Zero Java: android.app.NativeActivity + native_app_glue, both supplied by
//  the platform/NDK. See android/README.md for why this is the bring-up shape
//  and what would force a Java shell later.
// ============================================================================

#include <android_native_app_glue.h>
#include <android/log.h>

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/meta/Meta.hpp> // meta_module_init
#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/runtime/frame/FrameCoordinator.hpp>

#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/runtime/render/backend_host/RenderBackendHost.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
// Gate [6] — the editor's feature set, mirroring scene_cycle_stress_test.
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkinningOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TonemapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LineListOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/HzbOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/HighlightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SpatialCullOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LinearDepthOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SsaoOperation.ops.hpp>
#include <lux/engine/window/LuxWindow.hpp> // requiredVulkanInstanceExtensions

#include <sys/system_properties.h>
#include <unistd.h>
#include <cstdlib>

#include <string_view>
#include <thread>
#include <memory>
#include <utility>

#include <cstdint>
#include <vector>

#define LUX_TAG "luxsmoke"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LUX_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LUX_TAG, __VA_ARGS__)

namespace
{
    /// Android discards stdout/stderr for a normal app — nothing in logcat, no
    /// file, gone. The engine reports plenty through std::fprintf(stderr, ...)
    /// (every VK_FUNC_INVOKE failure, for one), so without this every one of
    /// those diagnostics is written and thrown away.
    ///
    /// This is not a nicety. The first device crash here was a null-pointer
    /// jump whose actual cause — vkCreateInstance returning
    /// VK_ERROR_LAYER_NOT_PRESENT — had been printed to stderr and discarded,
    /// leaving only a backtrace three frames downstream of the real fault.
    void redirectStdioToLogcat()
    {
        static int pipe_fds[2];
        static bool started = false;
        if (started)
            return;
        if (pipe(pipe_fds) != 0)
            return;
        started = true;

        // Unbuffered/line-buffered so output appears at the moment it is
        // written rather than whenever a 4 KiB buffer happens to fill — a
        // crash must not swallow the line that explains it.
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);

        std::thread([] {
            char line[1024];
            ssize_t n;
            while ((n = read(pipe_fds[0], line, sizeof(line) - 1)) > 0)
            {
                while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                    --n;
                line[n] = '\0';
                if (n > 0)
                    __android_log_write(ANDROID_LOG_INFO, "luxstdio", line);
            }
        }).detach();
    }

    /// [2] The reflection sidecar is a SEPARATE .so. Loading it is necessary
    /// but NOT sufficient: each generated .meta.cpp holds a file-scope
    /// MetaModuleRegistrar whose constructor only CHAINS its init function
    /// into a pending intrusive list — it registers nothing by itself. The
    /// list is drained by meta_module_init(), which every host must call once
    /// after all libraries are loaded (LuxEditor.cpp does it at startup).
    ///
    /// Getting this wrong is silent rather than loud: the catalogue is simply
    /// empty, and Scene::load then takes its "skip if type unknown" path for
    /// every component — the file loads, the entity count is right, and every
    /// entity owns nothing.
    bool gateComponentRegistry()
    {
        lux::ecs::ComponentTypeCatalog components;
        const auto registered = lux::ecs::initializeGeneratedMetadata(components);
        const auto all = components.all();
        LOGI("[2] ComponentTypeCatalog: %zu entries", all.size());
        if (!registered || all.empty())
        {
            LOGE("[2] FAIL: registry empty after meta_module_init() — either the "
                 "sidecar .so is absent from lib/<abi>/, or it loaded but its "
                 "registrars were stripped (check the -Wl,-u anchors)."
            );
            return false;
        }
        std::size_t shown = 0;
        for (const auto& e : all)
        {
            LOGI("[2]   %.*s", static_cast<int>(e.fullName().size()), e.fullName().data());
            if (++shown == 8)
            {
                LOGI("[2]   ... (%zu more)", all.size() - shown);
                break;
            }
        }
        LOGI("[2] PASS");
        return true;
    }

    /// [3] The numbers this whole trip exists for. maxBoundDescriptorSets was
    /// measured as 4 on desktop against Vulkan's REQUIRED MINIMUM (the local
    /// RTX part reports 32, so it could not be measured against the local
    /// device) — this is where we find out what the actual phone reports.
    bool gateVulkanLimits()
    {
        std::uint32_t loader_version = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion != nullptr)
            vkEnumerateInstanceVersion(&loader_version);
        LOGI(
            "[3] loader instance version: %u.%u.%u",
            VK_API_VERSION_MAJOR(loader_version),
            VK_API_VERSION_MINOR(loader_version),
            VK_API_VERSION_PATCH(loader_version)
        );

        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "lux_smoke";
        ai.apiVersion = loader_version; // ask for exactly what is offered

        const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &ai;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = exts;

        VkInstance instance = VK_NULL_HANDLE;
        if (const VkResult r = vkCreateInstance(&ici, nullptr, &instance); r != VK_SUCCESS)
        {
            LOGE("[3] FAIL: vkCreateInstance -> %d", int(r));
            return false;
        }

        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0)
        {
            LOGE("[3] FAIL: no physical device");
            vkDestroyInstance(instance, nullptr);
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());

        for (std::uint32_t i = 0; i < count; ++i)
        {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(devices[i], &p);
            const auto& L = p.limits;

            // UPDATE_AFTER_BIND limits are a SEPARATE budget from the plain
            // per-stage ones above, and the engine's bindless tables live
            // entirely inside it (that is why they carry UAB at all). Adreno
            // reports ~16.7M for the plain limits, which says nothing about
            // these — and it is these that clampLayoutMax() sizes against.
            VkPhysicalDeviceDescriptorIndexingProperties idx{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idx};
            vkGetPhysicalDeviceProperties2(devices[i], &p2);
            LOGI(
                "[3]   UAB maxDescriptorSetUpdateAfterBindSampledImages   = %u",
                idx.maxDescriptorSetUpdateAfterBindSampledImages
            );
            LOGI(
                "[3]   UAB maxPerStageDescriptorUpdateAfterBindSampledImg = %u",
                idx.maxPerStageDescriptorUpdateAfterBindSampledImages
            );
            LOGI(
                "[3]   UAB maxPerStageUpdateAfterBindResources           = %u",
                idx.maxPerStageUpdateAfterBindResources
            );
            LOGI(
                "[3]   UAB maxDescriptorSetUpdateAfterBindUniformBuffers  = %u",
                idx.maxDescriptorSetUpdateAfterBindUniformBuffers
            );
            LOGI(
                "[3] device[%u] '%s' api=%u.%u.%u",
                i,
                p.deviceName,
                VK_API_VERSION_MAJOR(p.apiVersion),
                VK_API_VERSION_MINOR(p.apiVersion),
                VK_API_VERSION_PATCH(p.apiVersion)
            );
            // The one that drives the whole >=4-set design.
            LOGI("[3]   maxBoundDescriptorSets            = %u", L.maxBoundDescriptorSets);
            // The independent per-stage constraint: merging sets does not
            // reduce these by a single descriptor.
            LOGI("[3]   maxPerStageDescriptorUniformBuffers= %u", L.maxPerStageDescriptorUniformBuffers);
            LOGI("[3]   maxPerStageDescriptorStorageBuffers= %u", L.maxPerStageDescriptorStorageBuffers);
            LOGI("[3]   maxPerStageDescriptorSampledImages = %u", L.maxPerStageDescriptorSampledImages);
            LOGI("[3]   maxPerStageDescriptorStorageImages = %u", L.maxPerStageDescriptorStorageImages);
            LOGI("[3]   maxPerStageResources               = %u", L.maxPerStageResources);

            // ── Hardcoded-assumption audit ────────────────────────────────
            // Each row pairs a constant the engine BAKES IN against the
            // device limit it must respect and against Vulkan's REQUIRED
            // MINIMUM. Two different failures live here and they are not the
            // same shape:
            //
            //   needs > device : breaks on THIS phone, right now.
            //   needs > min    : legal here, not portable — it will break on
            //                    some conformant device we have not held yet.
            //                    Only a table like this makes that visible,
            //                    because testing on hardware can never show
            //                    the absence of headroom you did not use.
            struct Row
            {
                const char* what;
                std::uint32_t needs, device, vk_min;
            };
            const Row rows[] = {
                // The widest color attachment count any pass in the editor-shaped
                // graph actually declares, measured from the graph dump (the
                // G-buffer group; everything else is 0 or 1).
                //
                // NOT RenderPassKey::kMaxColorAttachments (8) — that is the key
                // struct's ARRAY CAPACITY, which costs memory, not permission.
                // Reading it as "what the engine needs" is what made this look
                // like a violation when it never was. The real hazard was the
                // MERGE BUDGET reusing that capacity as its ceiling; it now
                // clamps to DeviceCaps::max_color_attachments.
                {"color attachments", 3u, L.maxColorAttachments, 4u},
                // cluster_scan.comp: single-workgroup Blelloch scan, now 128 wide.
                {"compute invocations", 128u, L.maxComputeWorkGroupInvocations, 128u},
                {"compute workgroup x", 128u, L.maxComputeWorkGroupSize[0], 128u},
                // kMaxShadowSlices (ShadowMapTypes.hpp) -> texture array layers.
                {"image array layers", 128u, L.maxImageArrayLayers, 256u},
                // kMaxDrawsPerBiasGroup (GpuDrivenMeshConsts.hpp).
                {"draw indirect count", 16384u, L.maxDrawIndirectCount, 65535u},
                // The widest pipeline, as measured by LayoutPlan on desktop.
                {"bound descriptor sets", 4u, L.maxBoundDescriptorSets, 4u},
            };
            for (const auto& r : rows)
            {
                const bool over_device = r.needs > r.device;
                const bool over_min = r.needs > r.vk_min;
                LOGI(
                    "[3]   %-22s needs=%-6u device=%-8u vkmin=%-6u %s%s",
                    r.what,
                    r.needs,
                    r.device,
                    r.vk_min,
                    over_device ? "OVER-DEVICE " : "ok ",
                    over_min ? "OVER-VKMIN" : ""
                );
            }
        }

        vkDestroyInstance(instance, nullptr);
        LOGI("[3] PASS");
        return true;
    }

    // ── Render-server plumbing for gates [4]-[6] ─────────────────────────
    // Process-scoped rather than a local of android_main, because the window
    // arrives (and is revoked) through the command callback, so the surface's
    // creation and destruction sites are in a different scope than the loop.
    struct Smoke
    {
        lux::runtime::RenderBackendHost<> render_host;
        lux::events::DomainEvents events;
        lux::events::EventPump* frame_pump{nullptr};
        std::unique_ptr<lux::runtime::FrameCoordinator> frames;
        lux::render::RenderProgramSession* session{nullptr};
        lux::render::RenderControlSession* control{nullptr};

        bool server_up{false};
        lux::render::RenderTargetLease target{};
        lux::render::RenderSceneId scene{};
        lux::render::ViewHandle view{};
        bool presented{false};
        std::uint32_t width{0};
        std::uint32_t height{0};
    };
    Smoke g;

    /// The fixture's await() pumps LuxWindow::pollEvents; there is no such
    /// window here, and the looper must NOT be pumped from inside a reply wait
    /// (we are already inside its callback). The reply travels on the channel,
    /// so pumping that alone is both necessary and sufficient.
    template <class T> bool awaitReq(lux::render::RenderRequest<T> req, T& out, const char* what)
    {
        auto result = g.control->syncCall(std::move(req));
        if (!result)
        {
            LOGE("[4] control channel stopped while awaiting %s", what);
            return false;
        }
        out = *result;
        return true;
    }

    template <class T> bool awaitFrameReq(lux::render::RenderRequest<T> req, T& out, const char* what)
    {
        if (!req.valid())
        {
            LOGE("[5] invalid frame request for %s", what);
            return false;
        }
        while (!req.isReady())
            if (!g.session->waitAndPumpReplies())
            {
                LOGE("[5] frame channel stopped while awaiting %s", what);
                return false;
            }
        if (req.failed())
        {
            LOGE("[5] frame request failed for %s", what);
            return false;
        }
        out = req.tryResult()->get();
        return true;
    }

    template <class Fn> bool runFrame(Fn&& record)
    {
        auto frame = g.frames->begin();
        if (!frame)
        {
            LOGE("[5] failed to open lexical frame");
            return false;
        }
        frame.record(std::forward<Fn>(record));
        return true;
    }

    void pumpFrame()
    {
        (void)runFrame([] {});
    }

    void dumpGraphToLogcat(const char* tag); // defined below, used by gate [6]

    /// [4] The whole reason RenderSurface::initFromNative exists. The handle
    /// travels as a POD on the CreateSurfaceTarget command straight to the
    /// render thread, which is where surface/swapchain creation is allowed to
    /// happen at all. Note what is NOT here: no LuxWindow, no attachToWindow.
    /// On Android LuxWindow::createVulkanSurface returns false BY DESIGN — the
    /// OS's window lifetime matches a surface, not a window object.
    /// Bring the render server up. ONCE per process — the Vulkan instance and
    /// device outlive any number of window acquire/revoke cycles, which is the
    /// whole point of not tying the surface to a window object.
    bool startServer()
    {
        const auto exts_span = lux::window::LuxWindow::requiredVulkanInstanceExtensions();
        std::vector<const char*> exts(exts_span.begin(), exts_span.end());
        for (const char* e : exts)
            LOGI("[4] instance extension: %s", e);

        lux::runtime::RenderBackendHost<>::Config cfg;
        cfg.instance_extensions = std::move(exts);
        cfg.enable_validation = true;
        cfg.validation_message_sink = [](std::uint32_t severity, std::string_view text) {
            if (severity >= 2)
                LOGE("[vk] %.*s", static_cast<int>(text.size()), text.data());
            else
                LOGI("[vk] %.*s", static_cast<int>(text.size()), text.data());
        };
        if (!g.render_host.start(std::move(cfg)))
        {
            LOGE("[4] server.init failed");
            return false;
        }

        g.session = &g.render_host.session();
        g.control = &g.render_host.controlSession();
        g.frame_pump = &g.events.createPump("android-smoke-frame");
        g.frames = std::make_unique<lux::runtime::FrameCoordinator>(*g.session, *g.frame_pump);
        g.server_up = true;
        return true;
    }

    /// Per window acquire. The handle is only valid between INIT_WINDOW and
    /// TERM_WINDOW, and a re-acquire hands back a DIFFERENT ANativeWindow —
    /// so this runs once per cycle while startServer() runs once per process.
    bool gateSurface(android_app* app)
    {
        g.width = static_cast<std::uint32_t>(ANativeWindow_getWidth(app->window));
        g.height = static_cast<std::uint32_t>(ANativeWindow_getHeight(app->window));
        LOGI("[4] ANativeWindow %ux%u", g.width, g.height);

        lux::render::TargetReadyReply t{};
        if (!awaitReq(
                g.control->createSurfaceRenderTarget(
                    reinterpret_cast<std::uint64_t>(app->window),
                    lux::math::Extent2u{g.width, g.height}
                ),
                t,
                "createSurfaceRenderTarget"))
            return false;
        if (!t.target.isValid())
        {
            LOGE("[4] FAIL: surface target invalid — vkCreateAndroidSurfaceKHR "
                 "or swapchain creation was rejected (see [vk] lines above)"
            );
            return false;
        }
        g.target = g.control->adoptTarget(t.target);
        LOGI("[4] PASS: VkSurfaceKHR + swapchain target created");
        return true;
    }

    /// [5] Present something. A scene with no content would clear to black,
    /// which is indistinguishable from "nothing was presented at all" — so
    /// draw one full-screen tinted quad and let `adb exec-out screencap`
    /// settle the question.
    bool gatePresent()
    {
        lux::render::SceneCreatedReply s{};
        if (!awaitReq(g.control->createScene("SmokeScene"), s, "createScene"))
            return false;
        g.scene = s.scene_id;
        g.control->setActiveScene(g.scene, true);

        lux::render::ViewCreatedReply v{};
        if (!awaitReq(g.control->addView(g.scene, {g.width, g.height}, "SmokeView"), v, "addView"))
            return false;
        g.view = v.view;
        g.control->setLayer(g.target.id(), 0, g.scene, g.view);

        lux::render::FeatureTypeRegisteredReply cam_reg{}, canvas_reg{};
        if (!awaitReq(
                g.control->registerFeatureType(lux::render::kViewCameraFeatureFactory),
                cam_reg,
                "registerFeatureType(ViewCamera)"))
            return false;
        if (!awaitReq(
                g.control->registerFeatureType(lux::render::kCanvas2DFeatureFactory),
                canvas_reg,
                "registerFeatureType(Canvas2D)"))
            return false;

        lux::render::FeatureAddedReply fa{};
        if (!awaitReq(
                g.control->addFeature(g.scene, cam_reg.feature_type_id, lux::render::ViewCameraCommTag{}),
                fa,
                "addFeature(ViewCamera)"))
            return false;
        if (!fa.feature.isValid())
        {
            LOGE("[5] FAIL: ViewCamera rejected");
            return false;
        }
        if (!awaitReq(
                g.control->addFeature(g.scene, canvas_reg.feature_type_id, lux::render::Canvas2DCommConfig{}),
                fa,
                "addFeature(Canvas2D)"))
            return false;
        if (!fa.feature.isValid())
        {
            LOGE("[5] FAIL: Canvas2D rejected");
            return false;
        }

        const auto cam_ops = lux::render::ViewCameraOperationIds::fromOps(cam_reg.ops, cam_reg.op_count);
        const auto canvas_ops = lux::render::Canvas2DOperationIds::fromOps(canvas_reg.ops, canvas_reg.op_count);

        const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const float eye[3] = {0, 0, 0};
        lux::render::Canvas2DProxy canvas(*g.session, canvas_ops);
        lux::render::Image2DInstanceData quad{};
        // Scale is in NDC units, where the visible range is -1..1 — so 1.0
        // covers half the screen in each axis, not all of it. Half is plenty:
        // the point is to be unmistakably not-black, and leaving a border
        // proves the clear and the draw are two different things.
        quad.m[0] = 1.0f;
        quad.m[3] = 1.0f;
        quad.m[4] = 0.0f;
        quad.m[5] = 0.0f; // centred
        // tint is 0xAABBGGRR (NOT ARGB) — this renders orange.
        quad.tint = 0xFF2080FFu;
        lux::render::Image2DSlotReply slot{};
        lux::render::RenderRequest<lux::render::Image2DSlotReply> add_image;
        if (!runFrame([&] {
                lux::render::viewCameraUpdateTransient(
                    lux::render::ViewCameraProxy(*g.session, cam_ops),
                    g.scene,
                    g.view,
                    view,
                    proj,
                    eye
                );
                add_image = lux::render::addImage(canvas, g.scene, quad, 0.0f);
            }))
            return false;
        if (!awaitFrameReq(std::move(add_image), slot, "addImage"))
            return false;

        for (int i = 0; i < 8; ++i)
        {
            if (!runFrame([&] {
                    lux::render::viewCameraUpdateTransient(
                        lux::render::ViewCameraProxy(*g.session, cam_ops),
                        g.scene,
                        g.view,
                        view,
                        proj,
                        eye
                    );
                }))
                return false;
        }
        g.presented = true;
        LOGI("[5] PASS: presented 8 frames with a tinted quad "
             "(verify with: adb exec-out screencap -p)"
        );
        return true;
    }

    /// [6] Attach the editor's whole feature set and report who is REFUSED.
    ///
    /// This is the gate the desktop cannot stand in for. Admission is decided
    /// by each feature's level_profiles row for the resolved tier, checked
    /// against the device's actual DeviceCaps — and on a desktop part every
    /// capability bit is present, so every feature is admitted there no matter
    /// what it declares. Only real silicon exercises the refusal path.
    ///
    /// A refusal is not a crash and not a downgrade: addFeature returns an
    /// invalid handle (LevelRequirementsUnmet / LevelProfileMissing). Report
    /// each one by name rather than failing the gate — "which features this
    /// phone cannot run" IS the result we came for.
    bool gateFeatureAdmission()
    {
        using namespace lux::render;

        lux::render::DeviceCaps caps{};
        DeviceCapsReply cr{};
        if (!awaitReq(g.control->queryDeviceCaps(caps), cr, "queryDeviceCaps"))
            return false;
        LOGI("[6] resolved feature_level=%u (0=Mobile 1=MobileHigh 2=Desktop)", cr.feature_level);

        ShadowMapCommConfig shmap_cfg{};
        shmap_cfg.enable_directional_csm = 1u;
        shmap_cfg.non_directional_shadow_max_distance = 0.0f;

        MeshShadowCommConfig mshsw_cfg{};
        mshsw_cfg.comm_config_version = kMeshShadowCommConfigVersion;
        mshsw_cfg.descriptor_layout_version = kMeshShadowDescriptorLayoutVersion;

        DeferredGBufferCommConfig gbuf_cfg{};
        gbuf_cfg.comm_config_version = kDeferredGBufferCommConfigVersion;
        gbuf_cfg.descriptor_layout_version = kDeferredGBufferDescriptorLayoutVersion;
        gbuf_cfg.extension_flags |= EGpuDrivenMeshExt::HZB;

        DeferredLightingCommConfig lit_cfg{};
        lit_cfg.read_mode = ELightingReadMode::SAMPLED;
        lit_cfg.enable_clustered = 1;

        SkyboxCommConfig sky_cfg{};
        TonemapCommConfig tm_cfg{.tone_map_op = ETonemapOperator::ACES_FILMIC, .exposure = 1.0f, .gamma = 2.2f};
        Grid3DCommConfig grid_cfg{};
        LineListTransientCommConfig line_cfg{};
        HighlightCommConfig hl_cfg{};

        // Bisect knob: attach only the first N features. A GPU hang names no
        // culprit by itself — the only way to find which feature's work kills
        // the device is to halve the set and re-run.
        //   adb shell setprop debug.lux.max_features <N>
        int max_features = 999;
        {
            char prop[PROP_VALUE_MAX] = {};
            if (__system_property_get("debug.lux.max_features", prop) > 0 && prop[0])
                max_features = atoi(prop);
        }
        // Skip mask: bit i set = do not attach feature i. A prefix limit can
        // only find "the last one that still works"; isolating an INTERACTION
        // needs the ability to remove a feature from the middle.
        unsigned long skip_mask = 0;
        {
            char prop[PROP_VALUE_MAX] = {};
            if (__system_property_get("debug.lux.skip_mask", prop) > 0 && prop[0])
                skip_mask = strtoul(prop, nullptr, 0);
        }
        LOGI("[6] max_features = %d  skip_mask = 0x%lx", max_features, skip_mask);

        std::uint32_t admitted = 0, refused = 0;
        int attached_count = 0;
        const auto attach = [&](const char* name, const FeatureFactory& factory, auto cfg) {
            const int idx = attached_count++;
            if (idx >= max_features)
                return;
            if (skip_mask & (1ul << idx))
            {
                LOGI("[6]   %-18s SKIPPED (idx %d)", name, idx);
                return;
            }
            FeatureTypeRegisteredReply reg{};
            if (!awaitReq(g.control->registerFeatureType(factory), reg, name))
            {
                LOGE("[6]   %-18s REGISTER FAILED", name);
                ++refused;
                return;
            }

            FeatureAddedReply fa{};
            if (!awaitReq(g.control->addFeature(g.scene, reg.feature_type_id, cfg), fa, name))
            {
                LOGE("[6]   %-18s ADD FAILED (channel)", name);
                ++refused;
                return;
            }

            if (fa.feature.isValid())
            {
                LOGI("[6]   %-18s admitted", name);
                ++admitted;
            }
            else
            {
                LOGE("[6]   %-18s REFUSED", name);
                ++refused;
            }
        };

        // Attach ORDER mirrors the standard feature plan's editor set. ViewCamera
        // and Canvas2D are already on the scene from gate [5].
        attach("Light", kLightFeatureFactory, LightCommTag{});
        attach("StandardMaterial", kMaterialFeatureFactory, MaterialCommTag{});
        attach("StandardMeshStack", kMeshStackFeatureFactory, MeshStackCommTag{});
        attach("Skinning", kSkinningFeatureFactory, SkinningCommConfig{});
        attach("ShadowMap", kShadowMapFeatureFactory, shmap_cfg);
        attach("MeshShadow", kMeshShadowFeatureFactory, mshsw_cfg);
        attach("DeferredGBuffer", kDeferredGBufferFeatureFactory, gbuf_cfg);
        attach("DeferredLighting", kDeferredLightingFeatureFactory, lit_cfg);
        attach("Skybox", kSkyboxFeatureFactory, sky_cfg);
        attach("Tonemap", kTonemapFeatureFactory, tm_cfg);
        attach("Grid", kGrid3DFeatureFactory, grid_cfg);
        attach("LineList", kLineListFeatureFactory, line_cfg);
        attach("Hzb", kHzbFeatureFactory, HzbCommTag{});
        attach("Highlight", kHighlightFeatureFactory, hl_cfg);
        attach("SpatialCull", kSpatialCullFeatureFactory, SpatialCullCommConfig{});
        attach("LinearDepth", kLinearDepthFeatureFactory, LinearDepthCommConfig{});
        attach("Ssao", kSsaoFeatureFactory, SsaoCommTag{});

        LOGI("[6] %u admitted, %u refused", admitted, refused);

        // Render with the full set attached: admission only proves the feature
        // was ALLOWED on, not that the graph it contributes actually compiles
        // and records on this hardware.
        //
        // Timed, and reported in buckets, because the first run showed
        // vkWaitForFences returning VK_TIMEOUT and ~750 ms/frame. Those two
        // have very different explanations — first-frame PSO compilation for
        // 17 features' worth of pipelines, versus a genuine per-frame stall —
        // and a mean over 8 frames cannot tell them apart. Render enough
        // frames to see whether it settles.
        constexpr int kFrames = 120;
        // Signed nanoseconds throughout: tv_nsec wraps every second, so
        // subtracting the fields separately in unsigned arithmetic underflows
        // whenever a frame straddles a second boundary. The first run of this
        // loop reported a frame at 1.8e16 us for exactly that reason.
        const auto nowNs = []() -> std::int64_t {
            timespec t{};
            clock_gettime(CLOCK_MONOTONIC, &t);
            return static_cast<std::int64_t>(t.tv_sec) * 1000000000ll + t.tv_nsec;
        };

        std::int64_t worst_us = 0;
        int slow_frames = 0;
        for (int i = 0; i < kFrames; ++i)
        {
            // Dump one frame BEFORE the known stall point: after the stall the
            // channel is wedged and the dump request would never come back.
            if (i == 5)
                dumpGraphToLogcat("pre-stall");

            const std::int64_t t0 = nowNs();
            pumpFrame();
            const std::int64_t us = (nowNs() - t0) / 1000;
            if (us > worst_us)
                worst_us = us;
            if (us > 50000 || i < 8)
            {
                if (us > 50000)
                    ++slow_frames;
                LOGI("[6]   frame %3d: %lld us", i, static_cast<long long>(us));
            }
            // A frame over a second is not slowness, it is a stall: the fence
            // wait is UINT64_MAX everywhere, so a timeout means the GPU never
            // signalled. Stop — continuing just renders onto a dead device and
            // buries the evidence under a hundred identical log lines.
            if (us > 1000000)
            {
                LOGE(
                    "[6] STALL at frame %d (%lld us) — stopping. Fence never signalled; "
                    "everything after this would run on a hung GPU.",
                    i,
                    static_cast<long long>(us)
                );
                return false;
            }
        }
        LOGI(
            "[6] %d frames: worst %lld us, %d frames over 50 ms",
            kFrames,
            static_cast<long long>(worst_us),
            slow_frames
        );
        LOGI("[6] PASS: rendered with the full editor feature set");
        return true;
    }

    /// Dump the compiled render graph to logcat, one line per line. Needed on
    /// device because the desktop dump cannot answer a device-specific
    /// question, and because a stall names no pass by itself.
    void dumpGraphToLogcat(const char* tag)
    {
        std::string buf(512 * 1024, '\0');
        lux::render::RenderGraphDumpReply rep{};
        if (!awaitReq(g.control->dumpRenderGraph(g.scene, buf.data(), buf.size()), rep, "dumpRenderGraph"))
            return;
        const std::size_t n = std::min<std::size_t>(rep.written, buf.size());
        LOGI("=== GRAPH DUMP (%s) %zu bytes ===", tag, n);
        std::size_t start = 0;
        while (start < n)
        {
            std::size_t end = buf.find('\n', start);
            if (end == std::string::npos || end > n)
                end = n;
            if (end > start)
                __android_log_print(
                    ANDROID_LOG_INFO,
                    "luxgraph",
                    "%.*s",
                    static_cast<int>(end - start),
                    buf.data() + start
                );
            start = end + 1;
        }
        LOGI("=== GRAPH DUMP END ===");
    }

    void teardownSurface()
    {
        // Two-phase destroy: the host may not drop the platform window until
        // the TargetReleased reply lands (surface-derived resources <= surface
        // <= platform window). On Android "dropping the window" is the OS
        // taking it back at TERM_WINDOW, so this wait is not optional.
        if (!g.server_up || !g.target)
            return;
        auto closing = g.target.close();
        if (closing)
        {
            lux::render::TargetReleasedReply r{};
            if (awaitReq(std::move(closing.value()), r, "surface target release"))
                LOGI("[lifecycle] surface target released");
        }
        g.presented = false;
    }

    void onAppCmd(android_app* app, std::int32_t cmd)
    {
        switch (cmd)
        {
        case APP_CMD_INIT_WINDOW:
            LOGI("[lifecycle] APP_CMD_INIT_WINDOW");
            if (app->window == nullptr)
                break;
            if (!g.server_up && !startServer())
            {
                LOGE("=== smoke result: FAIL at [4] (server init) ===");
                break;
            }
            if (g.target)
                break; // already have a surface for this window
            if (!gateSurface(app))
            {
                LOGE("=== smoke result: FAIL at [4] ===");
                break;
            }
            if (g.scene.isNull())
            {
                if (!gatePresent())
                {
                    LOGE("=== smoke result: FAIL at [5] ===");
                    break;
                }
                if (!gateFeatureAdmission())
                {
                    LOGE("=== smoke result: FAIL at [6] ===");
                    break;
                }
                LOGI("=== smoke result: PASS through [6] ===");
            }
            else
            {
                // Re-acquired after TERM_WINDOW: a NEW ANativeWindow, hence a
                // new surface, while the scene and its features live on. This
                // is precisely the cycle that makes a window-OWNED surface the
                // wrong model — the scene must not be rebuilt with the window.
                g.control->setLayer(g.target.id(), 0, g.scene, g.view);
                pumpFrame();
                LOGI("[lifecycle] surface rebuilt after re-acquire, scene kept");
            }
            break;
        case APP_CMD_TERM_WINDOW:
            LOGI("[lifecycle] APP_CMD_TERM_WINDOW");
            teardownSurface();
            break;
        default:
            break;
        }
    }
} // namespace

void
android_main(android_app* app)
{
    redirectStdioToLogcat(); // before anything that might report a failure
    LOGI("[1] PASS: .so loaded, android_main entered");

    app->onAppCmd = &onAppCmd;

    bool ok = gateComponentRegistry();
    if (ok)
        ok = gateVulkanLimits();
    LOGI("=== smoke result: %s ===", ok ? "PASS" : "FAIL");

    // Pump the looper regardless of the verdict: returning from android_main
    // tears the activity down, and the process would die before logcat is
    // drained. Staying alive also keeps the window lifecycle observable.
    while (true)
    {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0)
        {
            if (source != nullptr)
                source->process(app, source);
            if (app->destroyRequested != 0)
            {
                LOGI("[lifecycle] destroyRequested — exiting android_main");
                return;
            }
        }
    }
}
