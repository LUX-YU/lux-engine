#pragma once
// ============================================================================
//  RenderThreadOrchestrator — owns the editor's render thread + comm channel and
//  performs the GeneralRenderServer bring-up (the server + the editor's fixed
//  16-feature set: view-camera / light / material / mesh-stack / skinning /
//  shadow-map / mesh-shadow / gbuffer / deferred-lighting / skybox / tonemap /
//  grid / line-list / hzb / highlight / spatial-cull). start() spawns the thread
//  and blocks until the
//  server is up (or fails); the thread fills the caller's EditorRenderInfra +
//  ImGuiOperationIds (passed by reference, exactly as the old in-LuxEditor code
//  wrote those members) and flips a ready flag. The main thread then builds its
//  UIRenderSession from channel()/sync(). Extracted verbatim from
//  LuxEditor::renderThreadMain + the init() handshake.
//
//  Holds the window + config by reference (used only during start()'s server
//  bring-up) and writes render_infra_ / imgui_ops_ through references the editor
//  still owns. Private editor header (engine/editor/src/app — not installed).
// ============================================================================

#include <lux/engine/editor/app/LuxEditor.hpp>   // EditorConfig, EditorRenderInfra,
                                                 // comm channel/sync + ImGuiOperationIds types

#include <atomic>
#include <memory>
#include <thread>

namespace lux::editor
{
    class RenderThreadOrchestrator
    {
    public:
        RenderThreadOrchestrator(lux::window::LuxWindow&      window,
                                 const EditorConfig&          config,
                                 EditorRenderInfra&           render_infra_out,
                                 lux::ui::ImGuiOperationIds&  imgui_ops_out) noexcept;
        ~RenderThreadOrchestrator();

        RenderThreadOrchestrator(const RenderThreadOrchestrator&)            = delete;
        RenderThreadOrchestrator& operator=(const RenderThreadOrchestrator&) = delete;

        /// Create the comm channel + sync, spawn the render thread, and block
        /// until the server is up (returns true — render_infra / imgui_ops are
        /// now populated) or failed to start (returns false, thread joined).
        [[nodiscard]] bool start();

        /// Ask the render thread to stop and join it. Idempotent; also run by the
        /// destructor. Call while the borrowed window is still alive (the editor
        /// does so mid-shutdown, before tearing the window down).
        void requestStop() noexcept;

        const std::shared_ptr<lux::render::RenderProgramChannel<>>& channel() const noexcept
        { return channel_; }
        const std::shared_ptr<lux::render::RenderChannelSync>&      sync() const noexcept
        { return sync_; }

    private:
        // Background entry point for the render thread: server bring-up +
        // feature attach + scene/view creation, then the serve loop.
        void threadMain();

        lux::window::LuxWindow&      window_;        // used only during start()'s bring-up
        const EditorConfig&          config_;        // validation flag + viewport size
        EditorRenderInfra&           render_infra_;  // filled by threadMain (editor-owned)
        lux::ui::ImGuiOperationIds&  imgui_ops_;      // filled by threadMain (editor-owned)

        std::shared_ptr<lux::render::RenderProgramChannel<>> channel_;
        std::shared_ptr<lux::render::RenderChannelSync>      sync_;
        std::thread        render_thread_;
        std::atomic<bool>  server_ready_{false};
        std::atomic<bool>  server_failed_{false};
    };

} // namespace lux::editor
