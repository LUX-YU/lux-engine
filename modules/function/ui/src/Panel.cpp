#include <lux/engine/ui/Panel.hpp>
#include <imgui.h>

namespace lux::ui
{
	Panel::Panel(std::string title, std::array<float, 2> suggest_size)
		: title_(std::move(title)), suggest_size_(suggest_size), visible_(true) {}

	Panel::~Panel() = default;

	const std::string& Panel::title() const noexcept
	{
		return title_;
	}

	void Panel::setVisible(bool visible) noexcept
	{
		visible_ = visible;
	}

	void Panel::paint()
	{
		if (!visible_)
		{
			return;
		}
	}

	void Panel::onDelete()
	{
	}

	bool Panel::isVisible() const noexcept
	{
		return visible_;
	}

	int Panel::width() const noexcept
	{
		return ImGui::GetContentRegionAvail().x;
	}

	int Panel::height() const noexcept
	{
		return ImGui::GetContentRegionAvail().y;
	}
}
