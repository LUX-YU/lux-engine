#include <lux/engine/ui/Widget.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <imgui.h>

namespace lux::ui
{
	Widget::Widget(std::string title)
		: _title(std::move(title)), _visible(false){

	}

	Widget::~Widget()
	{
	}

	const std::string& Widget::title() const noexcept
	{
		return _title;
	}

	void Widget::setVisible(bool visible) noexcept
	{
		_visible = visible;
	}

	void Widget::paint()
	{
		if (!_visible)
		{
			return;
		}
	}

	bool Widget::isVisible() const noexcept
	{
		return _visible;
	}

	int Widget::width() const noexcept
	{
		return ImGui::GetContentRegionAvail().x;
	}

	int Widget::height() const noexcept
	{
		return ImGui::GetContentRegionAvail().y;
	}

	void Widget::setWindow(lux::window::LuxWindow* window)
	{
		_window = window;
	}
}