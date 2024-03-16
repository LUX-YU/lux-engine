#pragma once
#include <lux/engine/window/LuxWindow.hpp>
#include "Widget.hpp"

namespace lux::ui
{
	template<EGraphicAPI APIType> class WindowUI;

	template<> class WindowUI<EGraphicAPI::OpenGL3> : 
		public lux::window::LuxWindow, 
		public lux::event::EventHandler<WindowUI<EGraphicAPI::OpenGL3>>
	{
		template<typename T> friend class lux::event::EventHandler;

		using WindowSizeChangedEvent	 = lux::window::WindowSizeChangedEvent;
		using CursorPositionChangedEvent = lux::window::CursorPositionChangedEvent;
	public:
		/**
		 * @brief Construct a new Window with UI components 
		 * @param width The width of the window
		 * @param height The height of the window
		*/
		LUX_TOOLS_UI_PUBLIC WindowUI(int width, int height, std::string title);
		LUX_TOOLS_UI_PUBLIC ~WindowUI();

		template<typename T>
		void addWidget(std::unique_ptr<T> widget)
			requires std::is_base_of_v<Widget, T>
		{
			if constexpr (std::is_base_of_v<lux::event::EventHandler<T>, T>)
			{
				registerHandler<T>(widget.get());
			}
			add_widget(std::move(widget));
		}

		LUX_TOOLS_UI_PUBLIC void* uiContext();

		[[dont_invoke]] LUX_TOOLS_UI_PUBLIC std::unique_ptr<WindowSizeChangedEvent> handleEvent(std::unique_ptr<WindowSizeChangedEvent>);
		[[dont_invoke]] LUX_TOOLS_UI_PUBLIC std::unique_ptr<CursorPositionChangedEvent> handleEvent(std::unique_ptr<CursorPositionChangedEvent>);

	protected:

		LUX_TOOLS_UI_PUBLIC void add_widget(std::unique_ptr<Widget> widget);

		/**
		 * @brief Called every frame to update the UI
		*/
		LUX_TOOLS_UI_PUBLIC void newFrame() override;

	private:
		LUX_TOOLS_UI_PUBLIC void startDockingSpace();

		std::vector<std::unique_ptr<Widget>> _widgets;
	};
}