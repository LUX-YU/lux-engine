// ============================================================================
//  LuaConsole_EditorRework.cpp  – header + implementation (single translation
//                                 unit) for a *code‑editor* style Lua panel.
//
//  This completely replaces the previous prompt‑style LuaConsole with a
//  two‑pane layout:
//    ┌───────────────────────────────────────────────┐
//    │  multi‑line code editor (ImGui::InputTextMultiline
//    │  or any external colour text editor you drop │
//    ├───────────────────────────────────────────────┤
//    │  read‑only output log with coloured lines     │
//    └───────────────────────────────────────────────┘
//
//  • Press **Run** to execute the current buffer.
//  • Press **Clear** to clear the output pane.
//
//  Styling is still applied once via `applyStyle()` on first use.
// ============================================================================

#pragma once
#include <lux/engine/editor/visibility.h>
#include <lux/engine/script/lua/Lua.hpp>
#include <lux/engine/ui/Panel.hpp>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdarg>
#include <sstream>
#include <cstring>
#include <lux/engine/editor/app/FormatCompat.h>

namespace lux::asset { class AssetManager; }

// Editor-tier panel (was a lux::ui squatter; renamespaced in the GraphKit P2
// sweep — namespace follows the tier).
namespace lux::editor
{
    /**
     * @brief A two‑pane Lua scripting widget: big code editor + output pane.
     */
    class LUX_EDITOR_PUBLIC LuaConsole :
        public lux::ui::Panel,
        protected script::lua::ScriptEngine
    {
    public:
        explicit LuaConsole(std::string title = "Script Editor");
        ~LuaConsole() override;

        /** Flush the output pane */
        void clearLog();

        /**
         * Register the asset-address bindings (VP-P5, design doc §5.4 "API
         * shape locked now"):
         *   lux.find_asset(path)   -> uuid string ("" when unresolved)
         *   lux.asset_exists(path) -> bool
         *   lux.find_assets(prefix)-> sorted array of absolute vpaths
         * Relative paths sugar to /Game. NOTE: hand-registered sol2 today;
         * relocates verbatim into the shared runtime ScriptHost when the
         * ship-side Lua host lands (one registration, no editor variant).
         */
        void setAssetManager(std::shared_ptr<lux::asset::AssetManager> mgr);

        /** Append a formatted line to the output pane */
        template<typename... Args>
        void addLog(lux::format_string<Args...> fmt, Args&&... args)
        {
            items_.emplace_back(lux::format(fmt, std::forward<Args>(args)...));
        }

        using ScriptEngine::state; // expose lua_State* if needed

    protected:            // Widget overrides
		void beforePaint() override;
        void paint() override;
		void afterPaint() override;

    private:              // helpers
        /** Execute whatever is currently in code_buf_ */
        void runCurrentScript();

        /** Allow Ctrl+Enter / Ctrl+R shortcuts to run the script */
        static int TextEditShortcutCallback(ImGuiInputTextCallbackData* data);

        // --- Data ---
        std::string              code_buf_;           // multi‑line script buffer
        std::vector<std::string> items_;              // output lines
        bool                     auto_scroll_{ true };
        bool                     scroll_to_bottom_{ false };
        float                    split_ratio_{ 0.6f };
    };
}