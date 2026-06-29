#pragma once
// ============================================================================
//  UiAppHost.hpp — shared render-server bring-up for ImGui UI apps.
//
//  Every UI app (the editor, the launcher, ...) spins up a UIRenderServer on
//  its render thread the exact same way: build a ServerConfig (validation + the
//  UISystem's required Vulkan instance extensions), hand it the ImGui font
//  atlas, init(), then attachToWindow(). This collapses that verbatim prefix.
//
//  NOTE: the render-thread spawn / ready-spin / join and the app-specific
//  server-side scene setup deliberately stay in each app — they touch
//  app-private members and differ between apps. Only the common server
//  bring-up lives here.
// ============================================================================

#include <lux/engine/ui/UIRenderServer.hpp>   // UIRenderServer + Channel / RenderChannelSync / ServerConfig
#include <lux/engine/function/visibility_ui.h>

#include <memory>

namespace lux::window { class LuxWindow; }

namespace lux::ui
{
    /// Bring a UIRenderServer up to the point it is ready for app-specific
    /// server-side init: construct it on (@p channel, @p sync), init() it with
    /// Vulkan validation (@p enable_validation) + the UISystem's required
    /// instance extensions + the live ImGui font atlas, then attachToWindow().
    ///
    /// MUST run on the render thread — it reads the ImGui font atlas and touches
    /// the GPU. Returns the live server on success, or nullptr on failure (logged
    /// to stderr, tagged @p log_tag; the caller then flips its own server_failed_
    /// flag and returns).
    [[nodiscard]] LUX_FUNCTION_UI_PUBLIC std::unique_ptr<UIRenderServer>
    bringUpUIRenderServer(
        std::shared_ptr<lux::render::RenderProgramChannel<>> channel,
        std::shared_ptr<lux::render::RenderChannelSync>      sync,
        lux::window::LuxWindow&                              window,
        bool                                                 enable_validation,
        const char*                                          log_tag);

} // namespace lux::ui
