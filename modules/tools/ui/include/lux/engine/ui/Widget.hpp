#pragma once
#include <lux/engine/tools/visibility_ui.h>
#include <lux/engine/window/GLContext.hpp>
#include <string>

#if !defined LUX_TOOLS_UI_DLL_DISABLE
#include <imgui/imgui.h>
#endif

using EGraphicAPI = lux::window::EGraphicAPI;

namespace lux::ui
{
	class Widget
	{
		template<EGraphicAPI APIType> friend class WindowUI;
	public:
		LUX_TOOLS_UI_PUBLIC Widget(std::string title);
		LUX_TOOLS_UI_PUBLIC virtual ~Widget();

		LUX_TOOLS_UI_PUBLIC const std::string& title() const noexcept;

		LUX_TOOLS_UI_PUBLIC void setVisible(bool visible) noexcept;

		LUX_TOOLS_UI_PUBLIC bool isVisible() const noexcept;

		LUX_TOOLS_UI_PUBLIC int width() const noexcept;

		LUX_TOOLS_UI_PUBLIC int height() const noexcept;

#if !defined LUX_TOOLS_UI_DLL_DISABLE
		static void setContext(void* context)
		{
			ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
		}
#endif

	private:
		LUX_TOOLS_UI_PUBLIC virtual void paint(void* context);

		bool		_visible;
		std::string _title;
	};
}
