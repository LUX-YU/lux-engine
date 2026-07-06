#pragma once
// ============================================================================
//  DeviceRenderFixture.hpp — reusable device-level (gpu tier) render bring-up.
//
//  Stands up a REAL GeneralRenderServer on its own thread + a client RenderSession,
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
//      auto reg = fx.await(fx.session().registerFeatureType(kSomeFactory));
//      fx.await(fx.session().addFeature(sv.scene_id, reg.feature_type_id, cfg));
//      fx.flush(4);                                  // render a few frames
//      auto px = fx.readback(sv.scene_id, sv.view);  // BGRA8, width*height*4
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace lux::rendertest
{
    class DeviceRenderFixture
    {
    public:
        DeviceRenderFixture(std::uint32_t width, std::uint32_t height, const char* name)
            : glfw_inited_((glfwInit(), true))
            , width_(width)
            , height_(height)
            , window_(static_cast<int>(width), static_cast<int>(height), name)
        {
            channel_ = lux::render::RenderProgramChannel<>::create();
            sync_    = std::make_shared<lux::render::RenderChannelSync>();

            // GLFW-required instance extensions (the surface the server attaches to).
            std::vector<const char*> exts;
            {
                std::uint32_t count = 0;
                const char** raw = glfwGetRequiredInstanceExtensions(&count);
                for (std::uint32_t i = 0; i < count; ++i) exts.emplace_back(raw[i]);
            }

            server_thread_ = std::thread([this, exts]()
            {
                lux::render::GeneralRenderServer server(channel_, sync_);
                lux::render::ServerConfig cfg;
                cfg.instance_extensions = exts;
                if (auto r = server.init(std::move(cfg)); !r) { failed_.store(true); ready_.store(true); return; }
                if (auto r = server.attachToWindow(window_); !r) { failed_.store(true); ready_.store(true); return; }
                ready_.store(true);
                try { while (server.tick()) {} } catch (...) {}
            });

            while (!ready_.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

            if (!failed_.load(std::memory_order_acquire))
            {
                session_ = std::make_unique<lux::render::RenderSession>(channel_, sync_);
                session_->beginFrame({});
            }
        }

        ~DeviceRenderFixture()
        {
            if (sync_) sync_->requestStop();
            if (server_thread_.joinable()) server_thread_.join();
        }

        DeviceRenderFixture(const DeviceRenderFixture&)            = delete;
        DeviceRenderFixture& operator=(const DeviceRenderFixture&) = delete;

        /// false when no Vulkan device could be brought up — the test should skip (return 0).
        [[nodiscard]] bool ok() const noexcept
        { return !failed_.load(std::memory_order_acquire) && session_ != nullptr; }

        [[nodiscard]] lux::render::RenderSession&  session() noexcept { return *session_; }
        [[nodiscard]] lux::window::LuxWindow&      window()  noexcept { return window_; }
        [[nodiscard]] std::uint32_t                width()  const noexcept { return width_; }
        [[nodiscard]] std::uint32_t                height() const noexcept { return height_; }

        /// Submit BLOCKING + pump window events & replies until @p req resolves, then leave a
        /// fresh frame begun. A non-pumping blocking wait deadlocks the swapchain, and a
        /// non-blocking submit silently drops later commands when the ring is full.
        template <class T>
        T await(lux::render::RenderRequest<T> req)
        {
            session_->submitFrame(/*blocking=*/true);
            while (!req.isReady()) { window_.pollEvents(); session_->waitAndPumpReplies(); }
            T r = req.result();
            session_->beginFrame({});
            return r;
        }

        /// One blocking submit + pump (advances exactly one server tick); leaves a fresh frame.
        void flush()
        {
            session_->submitFrame(/*blocking=*/true);
            window_.pollEvents();
            session_->waitAndPumpReplies();
            session_->beginFrame({});
        }
        void flush(int n) { for (int i = 0; i < n; ++i) flush(); }

        struct SceneView
        {
            lux::render::RenderSceneId scene_id{};
            lux::render::ViewHandle    view{};
        };

        /// createScene + setActiveScene(true) + one width×height OFFSCREEN view.
        SceneView makeSceneWithView(const char* scene_name = "TestScene",
                                    const char* view_name  = "TestView")
        {
            const auto scene = await(session_->createScene(scene_name));
            await(session_->setActiveScene(scene.scene_id, true));
            const auto v = await(session_->addView(scene.scene_id, {width_, height_}, view_name));
            return { scene.scene_id, v.view };
        }

        /// Same, but the view is BOUND TO THE SWAPCHAIN so it presents to the (visible)
        /// window — for interactive / visual demos rather than offscreen readback.
        SceneView makeSceneWithSwapchainView(const char* scene_name = "TestScene",
                                             const char* view_name  = "TestView")
        {
            const auto scene = await(session_->createScene(scene_name));
            await(session_->setActiveScene(scene.scene_id, true));
            const auto v = await(session_->addView(scene.scene_id, {width_, height_}, view_name));
            session_->bindSwapchain(scene.scene_id, v.view);
            flush();   // let the bind take effect before the first content frame
            return { scene.scene_id, v.view };
        }

        /// True until the window's close button is pressed — the interactive-demo loop guard.
        [[nodiscard]] bool running() { window_.pollEvents(); return !window_.shouldClose(); }

        /// Blocking GPU→CPU color readback of @p view into a fresh width*height*4 BGRA8 buffer.
        /// The reply (status / dims / bytes_written / vk format) is kept in lastReadback().
        std::vector<std::uint8_t> readback(lux::render::RenderSceneId scene, lux::render::ViewHandle view)
        {
            std::vector<std::uint8_t> px(static_cast<std::size_t>(width_) * height_ * 4, 0);
            last_readback_ = await(session_->readbackView(scene, view, px.data(), px.size()));
            return px;
        }
        [[nodiscard]] const lux::render::ReadbackViewReply& lastReadback() const noexcept { return last_readback_; }

    private:
        bool                                                glfw_inited_;   // FIRST: glfwInit() before window_
        std::uint32_t                                       width_;
        std::uint32_t                                       height_;
        lux::window::LuxWindow                              window_;
        std::shared_ptr<lux::render::RenderProgramChannel<>> channel_;
        std::shared_ptr<lux::render::RenderChannelSync>     sync_;
        std::atomic<bool>                                   ready_{false};
        std::atomic<bool>                                   failed_{false};
        std::thread                                         server_thread_;
        std::unique_ptr<lux::render::RenderSession>         session_;
        lux::render::ReadbackViewReply                      last_readback_{};
    };

} // namespace lux::rendertest
