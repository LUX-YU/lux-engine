#include <lux/engine/editor/panels/LuaConsole.hpp>
#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/AssetVfs.hpp>
#include <lux/cxx/algorithm/string_operations.hpp>
#include <imgui_stdlib.h>
#include <sol/sol.hpp>
#include <algorithm>
#include <sstream>

#ifdef LUX_EDITOR_HAS_GAMEPLAY_LUA
// Generated aggregator (gameplay_meta's build include dir): registers every
// gameplay sol2 binding — the neutral-core + 3D (lux::pack) components +
// the LUX_FUNC() free functions (lux_debug_draw_line / lux_debug_draw_clear).
#include <ecs_lua_registration.hpp>
#include <pack_d3_lua_registration.hpp>
#endif

// ==========================================================================
//  Implementation
// ==========================================================================

// ------------------------------ Helpers ----------------------------------
namespace {
	// Forward declarations – helper functions live in anonymous namespace later
	static void drawLogLine(const std::string& line)
	{
		ImVec4 col;
		if (!line.empty() && line[0] == '#')                 // command marker
			col = { 0.35f, 0.80f, 1.00f, 1.00f };
		else if (line.rfind("[error]", 0) == 0)             // error
			col = { 1.00f, 0.30f, 0.30f, 1.00f };
		else                                                 // normal output
			col = { 0.80f, 0.80f, 0.80f, 1.00f };

		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextUnformatted(line.c_str());
		ImGui::PopStyleColor();
	}

	// Custom print that forwards to output pane --------------------------------
	static int l_print(lua_State* L)
	{
		auto* self = static_cast<::lux::editor::LuaConsole*>(lua_touserdata(L, lua_upvalueindex(1)));
		if (!self) return 0;

		std::ostringstream oss;
		int n = lua_gettop(L);
		for (int i = 1; i <= n; ++i)
		{
			size_t len;
			const char* str = luaL_tolstring(L, i, &len);
			if (i > 1) oss << '\t';
			oss.write(str, len);
			lua_pop(L, 1);
		}
		self->addLog("{}", oss.str());
		return 0;
	}

} // anonymous namespace

// ---------------------------- LuaConsole ---------------------------------
namespace lux::editor
{
	LuaConsole::LuaConsole(std::string title)
		: Panel(std::move(title))
	{
		clearLog();

		// pass errors to output pane
		setOnError([this](std::string_view e) { addLog("[error] {}", e); });

		lua_State* L = state();

		// Engine bindings FIRST: sol::lib::base defines a global `print`, so
		// the stdlib open + lux registration must run BEFORE the custom print
		// override below (ordering is load-bearing — do not swap).
		// Recipe proven by modules/core/script/test/meta_test.cpp.
		{
			sol::state_view lua(L);
			lua.open_libraries(sol::lib::base, sol::lib::math);
#ifdef LUX_EDITOR_HAS_GAMEPLAY_LUA
			LuxRegisterEcsMetas_LUA(lua);
			LuxRegisterPack_d3Metas_LUA(lua);
#endif
		}

		lua_pushlightuserdata(L, this);
		lua_pushcclosure(L, &l_print, 1); // upvalue = this*
		lua_setglobal(L, "print");

		// Starter code: the Milestone-D demo — draw a persistent debug line
		// in the Scene Viewport through the engine's lux.* bindings.
		code_buf_ =
			"-- lux API: lux.lux_debug_draw_line(x0,y0,z0, x1,y1,z1, r,g,b)\n"
			"--          lux.lux_debug_draw_clear()\n"
			"--          lux.find_asset(\"Materials/M_Box\")  -> uuid string (\"\" = none)\n"
			"--          lux.asset_exists(path) / lux.find_assets(prefix)\n"
			"lux.lux_debug_draw_line(0,0,0, 0,2,0, 1,0,0)\n"
			"print(\"line drawn\")\n"
			"for _, p in ipairs(lux.find_assets(\"/Game\")) do print(p) end\n";
	}

	LuaConsole::~LuaConsole() = default;

	void LuaConsole::setAssetManager(std::shared_ptr<lux::asset::AssetManager> mgr)
	{
		if (!mgr)
			return;

		sol::state_view lua(state());

		// The generated gameplay bindings own the `lux` table when present;
		// create it here when they're compiled out so the asset API is
		// reachable either way.
		sol::table lux_tbl;
		if (sol::object existing = lua["lux"]; existing.is<sol::table>())
			lux_tbl = existing.as<sol::table>();
		else
		{
			lux_tbl = lua.create_table();
			lua["lux"] = lux_tbl;
		}

		// Relative sugar: "Materials/M_Box" resolves under /Game (the design
		// doc's canonical form stays absolute-only — the sugar lives in the
		// binding, never in VirtualPath::parse).
		auto canonical = [](std::string path) -> std::string
		{
			if (!path.empty() && path.front() != '/')
				return "/Game/" + path;
			return path;
		};

		lux_tbl["find_asset"] =
			[mgr, canonical](const std::string& path) -> std::string
			{
				const auto id = mgr->findAssetByPath(canonical(path));
				return id.is_nil() ? std::string{} : uuids::to_string(id);
			};

		lux_tbl["asset_exists"] =
			[mgr, canonical](const std::string& path) -> bool
			{
				return !mgr->findAssetByPath(canonical(path)).is_nil();
			};

		lux_tbl["find_assets"] =
			[mgr, canonical](const std::string& prefix)
			{
				std::vector<std::string> out;
				if (const auto vfs = mgr->vfs())
				{
					const std::string want = canonical(prefix);
					vfs->enumerate(
						[&](const lux::asset::ProviderEntry& e)
						{
							if (e.vpath.rfind(want, 0) == 0)
								out.push_back(e.vpath);
						}
					);
					std::sort(out.begin(), out.end());
				}
				return sol::as_table(std::move(out));
			};
	}

	void LuaConsole::clearLog() { items_.clear(); }

	void LuaConsole::runCurrentScript()
	{
		if (code_buf_.empty()) return;

		addLog("# running script...");

		auto prog = parseScript(code_buf_.c_str());
		if (!prog)
		{
			addLog("[error] syntax error");
			return;
		}
		runScript(*prog);

		addLog("# done");
	}

	int LuaConsole::TextEditShortcutCallback(ImGuiInputTextCallbackData* data)
	{
		if ((data->EventFlag & ImGuiInputTextFlags_CallbackEdit) == 0)
			return 0;

		// detect Ctrl+Enter or Ctrl+R when editing multiline editor
		if (ImGui::IsKeyDown(ImGuiMod_Ctrl) &&
			(ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_R, false)))
		{
			auto* self = static_cast<LuaConsole*>(data->UserData);
			self->runCurrentScript();
		}
		return 0;
	}

	void LuaConsole::beforePaint()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));

		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.14f, 0.16f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.18f, 0.18f, 0.20f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.22f, 0.25f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.32f, 0.35f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.28f, 0.31f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.24f, 0.26f, 0.29f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.34f, 0.36f, 0.40f, 1.0f));
	}

	void LuaConsole::afterPaint()
	{
		ImGui::PopStyleColor(8);
		ImGui::PopStyleVar(5);
	}

	void LuaConsole::paint()
	{
		if (!isVisible()) return;

		// ------------------------------------------------ toolbar
		if (ImGui::Button("Run")) runCurrentScript();
		ImGui::SameLine(); if (ImGui::Button("Clear Output")) clearLog();
		ImGui::SameLine(); ImGui::Checkbox("Auto scroll", &auto_scroll_);

		// ------------------------------------------------ get region metrics
		const float splitter_sz = 4.0f;
		const float avail_y = ImGui::GetContentRegionAvail().y;
		const ImVec2 region_min = ImGui::GetCursorScreenPos(); // top‑left of editor/log region in screen coords

		// compute heights from stored ratio (initial layout & non‑drag frames)
		float editor_h = avail_y * split_ratio_ - splitter_sz * 0.5f;
		float output_h = avail_y - editor_h - splitter_sz;

		// ---------------------------- editor pane
		ImGuiInputTextFlags ed_flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackEdit;
		ImGui::InputTextMultiline("##CodeEditor", &code_buf_, ImVec2(-FLT_MIN, editor_h),
			ed_flags, &TextEditShortcutCallback, (void*)this);

		// ---------------------------- splitter bar (invisible button)
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 0.35f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.5f, 0.5f, 0.50f));
		ImGui::Button("##Splitter", ImVec2(-FLT_MIN, splitter_sz));
		ImGui::PopStyleColor(3);

		if (ImGui::IsItemActive())
		{
			// translate absolute mouse Y to a ratio in [0,1]
			const float mouse_y = ImGui::GetIO().MousePos.y;
			const float new_ratio = (mouse_y - region_min.y) / avail_y;
			split_ratio_ = std::clamp(new_ratio, 0.05f, 0.95f);

			// recompute heights for this frame so the visual feedback is immediate
			editor_h = avail_y * split_ratio_ - splitter_sz * 0.5f;
			output_h = avail_y - editor_h - splitter_sz;
		}

		// ---------------------------- output pane
		ImGui::BeginChild("Output", ImVec2(0, output_h), false, ImGuiWindowFlags_HorizontalScrollbar);
		for (const auto& l : items_) drawLogLine(l);
		if (scroll_to_bottom_ || (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
			ImGui::SetScrollHereY(1.0f);
		scroll_to_bottom_ = false;
		ImGui::EndChild();
	}
}