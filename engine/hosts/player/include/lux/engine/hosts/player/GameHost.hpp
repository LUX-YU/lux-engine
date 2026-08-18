#pragma once
/**
 * @file GameHost.hpp
 * @brief Reference desktop adapter: GLFW window, input snapshots and event loop.
 *
 * Runtime composition is owned by the installed GameApplication component,
 * which is also used by Android and can be linked by an external game project.
 * This reference host intentionally owns only the desktop platform policy.
 *
 * Deliberately NOT here: ImGui/editor anything or authoring/loose-file input;
 * window resize policy (v1 windows are fixed-size; minimize skips frames).
 */

#include <lux/engine/hosts/player/visibility.h>
#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>
#include <lux/engine/function/render/Capacity.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace lux::extensions
{
    class EngineExtensions;
}

namespace lux::game
{
    struct GameHostConfig
    {
        std::string title  = "Lux Player";
        uint32_t    width  = 1280;
        uint32_t    height = 720;

        /// Cooked game pak mounted at /Game.
        std::filesystem::path pak_file;

        /// Optional cooked base-content pak mounted at the legacy /Engine VFS root.
        std::filesystem::path base_pak_file;

        /// EntityScene virtual path inside the pak ("Scenes/BigDemo"). Empty
        /// selects the pak's single ENTITY_SCENE entry.
        std::string scene_vpath;

        /// Writable save root selected by the platform adapter.
        std::filesystem::path save_root;

        lux::render::CapacityRequest capacity_request{};

        /// Enable the Vulkan validation layer (its messages go to stderr).
        bool enable_validation = false;

        /// Dev diagnostics: dump the compiled render graph to stderr shortly
        /// after bring-up (which passes were culled and why — the same
        /// dumpRenderGraph the Android bring-up harness relies on).
        bool dump_graph = false;

        /// Explicit deployed runtime modules from LaunchManifest.
        std::vector<lux::extensions::ExtensionModuleRequirement> extensions;
    };

    /// One object = the reference desktop shell. main() remains three lines.
    class LUX_GAME_HOST_PUBLIC GameHost
    {
    public:
        GameHost();
        ~GameHost();

        GameHost(const GameHost&)            = delete;
        GameHost& operator=(const GameHost&) = delete;

        [[nodiscard]] bool init(const GameHostConfig& cfg);

        /// The frame loop. Returns the process exit code (0 = closed
        /// normally). Requires a successful init().
        [[nodiscard]] int run();

        /// Reverse-order teardown. Safe to call more than once; the
        /// destructor calls it as a safety net.
        void shutdown() noexcept;

        /// Dev diagnostics: dump the scene's compiled render graph to stderr
        /// (run() calls this once when Config::dump_graph is set; callable
        /// any time between init() and shutdown()).
        void dumpRenderGraph();

        [[nodiscard]] lux::extensions::EngineExtensions& extensions() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::game
