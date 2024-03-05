#include <lux/engine/window/Subwindow.hpp>
#include <imgui/imgui.h>

namespace lux::window
{
	Subwindow::Subwindow(std::string title)
		: _title(std::move(title)) {

	}

	Subwindow::~Subwindow()
	{
	}

	const std::string& Subwindow::title() const noexcept
	{
		return _title;
	}

	void Subwindow::setVisible(bool visible) noexcept
	{
		_visible = visible;
	}

	void Subwindow::paint()
	{
		if (!_visible)
		{
			return;
		}

		ImGui::Begin(_title.c_str(), &_visible);

		ImGui::End();
	}

	bool Subwindow::isVisible() const noexcept
	{
		return _visible;
	}

	int Subwindow::width() const noexcept
	{
		return ImGui::GetContentRegionAvail().x;
	}

	int Subwindow::height() const noexcept
	{
		return ImGui::GetContentRegionAvail().y;
	}
}