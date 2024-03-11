#pragma once
#include <lux/engine/window/LuxWindow.hpp>
#include "Widget.hpp"

namespace lux::ui
{
	template<EGraphicAPI APIType> class WindowUI;

	template<> class WindowUI<EGraphicAPI::OpenGL3> : public lux::window::LuxWindow
	{
	public:
		/**
		 * @brief Construct a new Window with UI components
		 * @param width The width of the window
		 * @param height The height of the window
		*/
		LUX_TOOLS_UI_PUBLIC WindowUI(int width, int height);
		LUX_TOOLS_UI_PUBLIC ~WindowUI();

		LUX_TOOLS_UI_PUBLIC void addSubwindow(std::unique_ptr<Widget> widget);

		LUX_TOOLS_UI_PUBLIC void* uiContext();

	protected:

		/**
		 * @brief Called every frame to update the UI
		*/
		LUX_TOOLS_UI_PUBLIC void newFrame() override;

	private:

		LUX_TOOLS_UI_PUBLIC void window_size_change_callback(int width, int height);

		std::vector<std::unique_ptr<Widget>> widgets;

		float _delta_time = 0.0f;
		float _last_frame = 0.0f;
	};
}