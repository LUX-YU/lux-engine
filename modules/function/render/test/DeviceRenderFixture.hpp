#pragma once
// ============================================================================
//  DeviceRenderFixture.hpp — reusable device-level (gpu tier) render bring-up.
//
//  Stands up a REAL GeneralRenderServer on its own thread + a client RenderFrameSession,
//  attached to a small window (needed only for the Vulkan surface — rendering targets
//  an OFFSCREEN view, so nothing is shown and the test never waits for interaction).
//  Replaces the ~120-line server-init boilerplate that every gpu test copy-pastes
//  (readback_test / graph_dump_stability_test / deferred_* / feature_view_lifecycle …),
//  which UNFINISHED-WORK.md repeatedly flags as the missing shared fixture blocking
//  device-level unit tests (PR-1/2/3/4 assertions, the Canvas2D visual gate, …).
//
//  Skips gracefully when no Vulkan device is present: ok() == false → the test should
//  print a skip line and return 0 (never fail on a headless CI box without a GPU).
//
//  Usage:
//      lux::rendertest::DeviceRenderFixture fx(64, 64, "my_test");
//      if (!fx.ok()) return 0;                       // no Vulkan → skip
//      auto sv = fx.makeSceneWithView();             // scene + offscreen view
//      auto reg = fx.awaitControl(fx.control().registerFeatureType(kSomeFactory));
//      fx.awaitControl(fx.control().addFeature(sv.scene_id, reg.feature_type_id, cfg));
//      fx.flush(4);                                  // render a few frames
//      auto px = fx.readback(sv);                    // BGRA8, width*height*4
// ============================================================================

#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>

#include <lux/engine/window/LuxWindow.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace lux::rendertest
{
    class DeviceRenderFixture
    {
    public:
        /// Optional server knobs. Defaults preserve the historical fixture shape
        /// (no validation). `validation_errors` (with enable_validation) turns
        /// validation output into an assertable counter — read it AFTER the
        /// fixture is destroyed so instance/device-destroy leak reports count too.
        struct Options
        {
            bool              enable_validation{false};
            std::atomic<int>* validation_errors{nullptr};
            /// Requested feature-level tier. The server resolves
            /// min(device-achievable, this), so on a desktop box asking for
            /// Mobile actually GETS Mobile — which is the only way to exercise
            /// the mobile implementation variants (bound-SSBO cull instead of
            /// BDA cull, gl_Layer caster instead of multiview) without a
            /// phone. Those are different pipelines, so anything measured per
            /// pipeline — descriptor set counts above all — has to be measured
            /// at the tier it will actually run at.
            lux::render::EFeatureLevel preferred_level{lux::render::EFeatureLevel::Desktop};
            lux::deployment::RuntimeCapacityRequest capacity_request{};
        };

        DeviceRenderFixture(std::uint32_t width, std::uint32_t height, const char* name,
                            Options opts = {})
            : glfw_inited_(initWindowBackend())
            , width_(width)
            , height_(height)
            , window_(static_cast<int>(width), static_cast<int>(height), name)
        {
            channel_ = lux::render::RenderFrameChannel<>::create();
            control_channel_ = lux::render::RenderControlChannel<>::create();
            upload_channel_ = lux::render::RenderUploadChannel<>::create();
            sync_    = std::make_shared<lux::render::RenderChannelSync>();

            // Backend-required instance extensions (the surface the server attaches to).
            std::vector<const char*> exts;
            {
                const auto span = lux::window::LuxWindow::requiredVulkanInstanceExtensions();
                exts.assign(span.begin(), span.end());
            }

            server_thread_ = std::thread([this, exts, opts]()
            {
                lux::render::GeneralRenderServer server(
                    channel_, control_channel_, upload_channel_, sync_);
                lux::render::ServerConfig cfg;
                cfg.instance_extensions      = exts;
                cfg.enable_validation        = opts.enable_validation;
                cfg.validation_error_counter = opts.validation_errors;
                cfg.preferred_level          = opts.preferred_level;
                cfg.capacity_request         = opts.capacity_request;
                if (auto r = server.init(std::move(cfg)); !r) { failed_.store(true); ready_.store(true); return; }
                if (auto r = server.attachToWindow(window_); !r) { failed_.store(true); ready_.store(true); return; }
                ready_.store(true);
                while (server.tick()) {}
                // Liveness: if the loop ended WITHOUT the fixture asking (device loss,
                // an exception swallowed above, a server-side fatal), any await() is
                // blocked on a reply that will never come. Mark the death and set the
                // channel stopping so reply_cv waiters wake and fail FAST with a message
                // instead of dying as an opaque ctest TIMEOUT.
                if (!stop_requested_.load(std::memory_order_acquire))
                {
                    server_died_.store(true, std::memory_order_release);
                    sync_->requestStop();
                }
            });

            while (!ready_.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

            if (!failed_.load(std::memory_order_acquire))
            {
                session_ = std::make_unique<lux::render::RenderFrameSession>(channel_, sync_);
                session_->setErrorEventHandler(
                    [](const lux::render::ErrorEventBatchReply&) {},
                    [](const lux::render::RenderErrorEvent& event)
                    {
                        const auto message = lux::render::formatRenderError(
                            lux::render::renderErrorRegistry(),
                            event.error
                        );
                        std::fprintf(
                            stderr,
                            "[DeviceRenderFixture] render error: %s\n",
                            message.c_str()
                        );
                    }
                );
                control_ = std::make_unique<lux::render::RenderControlSession>(
                    control_channel_, sync_);
                upload_ = std::make_unique<lux::render::RenderUploadSession>(
                    upload_channel_, sync_);
                direct_upload_state_ = std::make_shared<DirectUploadState>();
                direct_upload_state_->session = upload_.get();
                (void)session_->beginFrame({});
            }
        }

        ~DeviceRenderFixture()
        {
            if (direct_upload_state_)
                direct_upload_state_->session = nullptr;
            stop_requested_.store(true, std::memory_order_release);   // an expected stop, not a death
            if (sync_) sync_->requestStop();
            if (server_thread_.joinable()) server_thread_.join();
        }

        DeviceRenderFixture(const DeviceRenderFixture&)            = delete;
        DeviceRenderFixture& operator=(const DeviceRenderFixture&) = delete;

        /// false when no Vulkan device could be brought up — the test should skip (return 0).
        [[nodiscard]] bool ok() const noexcept
        { return !failed_.load(std::memory_order_acquire) && session_ != nullptr; }

        [[nodiscard]] lux::render::RenderFrameSession&  session() noexcept { return *session_; }
        [[nodiscard]] lux::render::RenderControlSession& control() noexcept
        {
            return *control_;
        }
        [[nodiscard]] lux::render::RenderUploadSession& upload() noexcept
        {
            return *upload_;
        }

        /// Low-level tests deliberately make their test thread the upload
        /// coordinator.  This facade exercises the same owning packet/client
        /// protocol as production without installing an engine AsyncRuntime.
        /// Engine-level tests use AsyncTestServices instead.
        [[nodiscard]] lux::render::RenderUploadClient
        uploadClientForTest() const noexcept
        {
            return lux::render::RenderUploadClient::bind(
                direct_upload_state_,
                +[](void* opaque,
                    std::shared_ptr<
                        lux::render::detail::PreparedUpload> prepared) noexcept
                    -> lux::render::UploadSubmitNoReplyResult
                {
                    auto* state = static_cast<DirectUploadState*>(opaque);
                    if (!state || !state->session || !prepared)
                    {
                        return lux::cxx::unexpected(
                            lux::render::ERenderUploadSubmitError::STOPPING);
                    }
                    if (prepared->expected_reply_type ==
                        lux::render::kInvalidTypeId)
                    {
                        return state->session->trySubmitPreparedNoReply(
                            prepared->packet);
                    }
                    return state->session->trySubmitPrepared(
                        prepared->packet,
                        prepared->expected_reply_type,
                        std::move(prepared->callback));
                });
        }
        [[nodiscard]] std::shared_ptr<lux::render::RenderChannelSync>
        sync() const noexcept
        {
            return sync_;
        }
        [[nodiscard]] lux::window::LuxWindow&      window()  noexcept { return window_; }
        [[nodiscard]] std::uint32_t                width()  const noexcept { return width_; }
        [[nodiscard]] std::uint32_t                height() const noexcept { return height_; }

        /// Pump every independent reply lane. Tests that drive a whole engine
        /// safe point must not assume a frame reply also carries control or
        /// persistent-upload completions.
        void pumpReplies()
        {
            session_->pumpReplies();
            control_->pumpReplies();
            if (!upload_->coordinatorOwned())
                upload_->pumpReplies();
        }

        /// Submit BLOCKING + pump window events & replies until @p req resolves, then leave a
        /// fresh frame begun. A non-pumping blocking wait deadlocks the swapchain, and a
        /// non-blocking submit silently drops later commands when the ring is full.
        /// Liveness: waitAndPumpReplies() returning false means the channel is stopping and
        /// the reply can never arrive (RenderFrameSession::syncCall honours the same signal) —
        /// fail fast with a diagnostic instead of hanging into the ctest timeout.
        template <class T>
        T await(lux::render::RenderRequest<T> req)
        {
            submitStagedFrame("await submit");
            while (!req.isReady())
            {
                window_.pollEvents();
                const bool channel_alive = session_->waitAndPumpReplies();
                control_->pumpReplies();
                if (!upload_->coordinatorOwned())
                    upload_->pumpReplies();
                if (req.isReady()) break;
                if (!channel_alive || server_died_.load(std::memory_order_acquire))
                    dieServerLost("await");
            }
            T r = req.tryResult()->get();
            (void)session_->beginFrame({});
            return r;
        }

        /// Wait for a control-plane request without publishing a FrameProgram.
        /// This is the regression seam proving scene/resource control remains
        /// live while frame production is paused or minimized.
        template <class T>
        T awaitControl(lux::render::RenderRequest<T> req)
        {
            while (!req.isReady())
            {
                window_.pollEvents();
                const bool channel_alive = control_->waitAndPumpReplies();
                session_->pumpReplies();
                if (!upload_->coordinatorOwned())
                    upload_->pumpReplies();
                if (req.isReady()) break;
                if (!channel_alive || server_died_.load(std::memory_order_acquire))
                    dieServerLost("awaitControl");
            }
            return req.tryResult()->get();
        }

        /// Wait for an upload-plane request without publishing a FrameProgram.
        /// The transfer result, graphics-finalize control submit, and reply must
        /// all make progress from upload/transfer epochs alone.
        template <class T>
        T awaitUpload(lux::render::RenderRequest<T> req)
        {
            while (!req.isReady())
            {
                window_.pollEvents();
                const bool channel_alive = upload_->waitAndPumpReplies();
                session_->pumpReplies();
                control_->pumpReplies();
                if (req.isReady()) break;
                if (!channel_alive || server_died_.load(std::memory_order_acquire))
                    dieServerLost("awaitUpload");
            }
            return req.tryResult()->get();
        }

        /// One blocking submit + pump (advances exactly one server tick); leaves a fresh frame.
        void flush()
        {
            submitStagedFrame("flush submit");
            window_.pollEvents();
            if (!session_->waitAndPumpReplies() || server_died_.load(std::memory_order_acquire))
                dieServerLost("flush");
            control_->pumpReplies();
            if (!upload_->coordinatorOwned())
                upload_->pumpReplies();
            if (!session_->beginFrame({}))
                dieServerLost("flush begin");
        }
        void flush(int n) { for (int i = 0; i < n; ++i) flush(); }

        struct SceneView
        {
            lux::render::RenderSceneId  scene_id{};
            lux::render::ViewHandle     view{};
            lux::render::RenderTargetId target{};  ///< offscreen 目标(swapchain 视图无)
        };

        /// createScene + setActiveScene(true) + one width×height OFFSCREEN view,
        /// wired to an explicit offscreen render target (views no longer imply
        /// targets — the fixture does the createOffscreenRenderTarget + setLayer
        /// handshake so tests can readback(sv) directly).
        SceneView makeSceneWithView(const char* scene_name = "TestScene",
                                    const char* view_name  = "TestView")
        {
            const auto scene = awaitControl(control_->createScene(scene_name));
            awaitControl(control_->setActiveScene(scene.scene_id, true));
            const auto v = awaitControl(
                control_->addView(scene.scene_id, {width_, height_}, view_name));
            const auto t = awaitControl(
                control_->createOffscreenRenderTarget({width_, height_}));
            control_->setLayer(t.target, 0, scene.scene_id, v.view);
            flush();   // let the layer wiring take effect before the first content frame
            return { scene.scene_id, v.view, t.target };
        }

        /// Configurable-scene counterpart used by semantic tests that need an
        /// exact attachment format (for example, deferred LDR readback).
        SceneView makeSceneWithView(
            const lux::render::RenderControlSession::CreateSceneConfig& config,
            const char* view_name)
        {
            const auto scene = awaitControl(control_->createScene(config));
            awaitControl(control_->setActiveScene(scene.scene_id, true));
            const auto v = awaitControl(
                control_->addView(scene.scene_id, {width_, height_}, view_name));
            const auto t = awaitControl(
                control_->createOffscreenRenderTarget({width_, height_}));
            control_->setLayer(t.target, 0, scene.scene_id, v.view);
            flush();
            return {scene.scene_id, v.view, t.target};
        }

        /// Same, but the view is BOUND TO THE SWAPCHAIN so it presents to the (visible)
        /// window — for interactive / visual demos rather than offscreen readback.
        SceneView makeSceneWithSwapchainView(const char* scene_name = "TestScene",
                                             const char* view_name  = "TestView")
        {
            const auto scene = awaitControl(control_->createScene(scene_name));
            awaitControl(control_->setActiveScene(scene.scene_id, true));
            const auto v = awaitControl(
                control_->addView(scene.scene_id, {width_, height_}, view_name));
            control_->bindSwapchain(scene.scene_id, v.view);
            flush();   // let the bind take effect before the first content frame
            return { scene.scene_id, v.view };
        }

        /// True until the window's close button is pressed — the interactive-demo loop guard.
        [[nodiscard]] bool running() { window_.pollEvents(); return !window_.shouldClose(); }

        /// Blocking GPU→CPU color readback of @p sv's offscreen target into a fresh
        /// width*height*4 BGRA8 buffer.
        /// The reply (status / dims / bytes_written / vk format) is kept in lastReadback().
        std::vector<std::uint8_t> readback(const SceneView& sv)
        {
            std::vector<std::uint8_t> px(static_cast<std::size_t>(width_) * height_ * 4, 0);
            last_readback_ = awaitControl(
                control_->readbackTarget(sv.target, px.data(), px.size()));
            return px;
        }
        [[nodiscard]] const lux::render::ReadbackTargetReply& lastReadback() const noexcept { return last_readback_; }

    private:
        struct DirectUploadState final
        {
            lux::render::RenderUploadSession* session{nullptr};
        };

        /// Publish the staged FrameProgram without turning normal bounded-ring
        /// backpressure into a fatal error. Observe the epoch before pumping to
        /// close the publication-before-wait race; every released request slot
        /// advances the same progress domain.
        void submitStagedFrame(const char* where)
        {
            for (;;)
            {
                if (session_->trySubmitFrame())
                    return;
                if (session_->isStopping() ||
                    server_died_.load(std::memory_order_acquire))
                {
                    dieServerLost(where);
                }

                const auto observed = session_->observeProgress();
                window_.pollEvents();
                pumpReplies();

                if (session_->trySubmitFrame())
                    return;
                if (session_->isStopping() ||
                    server_died_.load(std::memory_order_acquire))
                {
                    dieServerLost(where);
                }
                session_->waitForProgress(observed);
            }
        }

        /// The server thread stopped WITHOUT the fixture asking — the test cannot make
        /// progress, so await()/flush() abort with a message rather than hang.
        [[noreturn]] void dieServerLost(const char* where)
        {
            // A terminal frame error is published just before the server closes
            // the shared channel. Drain once without waiting so the structured
            // diagnostic is not hidden by the liveness guard below.
            if (session_)
                session_->pumpReplies();
            std::fprintf(stderr,
                "[DeviceRenderFixture] render server thread died / channel stopped during %s — "
                "a pending reply can never arrive; aborting instead of hanging into the ctest timeout.\n",
                where);
            std::abort();
        }

        /// FIRST member: initializes the window backend before window_ is constructed.
        /// requiredVulkanInstanceExtensions() idempotently runs glfwInit() internally.
        static bool initWindowBackend()
        {
            return !lux::window::LuxWindow::requiredVulkanInstanceExtensions().empty();
        }

        bool                                                glfw_inited_;   // FIRST: backend init before window_
        std::uint32_t                                       width_;
        std::uint32_t                                       height_;
        lux::window::LuxWindow                              window_;
        std::shared_ptr<lux::render::RenderFrameChannel<>> channel_;
        std::shared_ptr<lux::render::RenderControlChannel<>> control_channel_;
        std::shared_ptr<lux::render::RenderUploadChannel<>> upload_channel_;
        std::shared_ptr<lux::render::RenderChannelSync>     sync_;
        std::atomic<bool>                                   ready_{false};
        std::atomic<bool>                                   failed_{false};
        std::atomic<bool>                                   stop_requested_{false};
        std::atomic<bool>                                   server_died_{false};
        std::thread                                         server_thread_;
        std::unique_ptr<lux::render::RenderFrameSession>         session_;
        std::unique_ptr<lux::render::RenderControlSession>  control_;
        std::unique_ptr<lux::render::RenderUploadSession>   upload_;
        std::shared_ptr<DirectUploadState>                  direct_upload_state_;
        lux::render::ReadbackTargetReply                    last_readback_{};
    };

} // namespace lux::rendertest
