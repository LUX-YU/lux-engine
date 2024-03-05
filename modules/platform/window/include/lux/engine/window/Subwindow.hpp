#pragma once
#include <lux/engine/platform/visibility.h>
#include <string>

namespace lux::window
{
	class Subwindow
	{
		friend class LuxWindowImpl;
	public:
		LUX_PLATFORM_PUBLIC Subwindow(std::string title);
		LUX_PLATFORM_PUBLIC virtual ~Subwindow();

		LUX_PLATFORM_PUBLIC const std::string& title() const noexcept;

		LUX_PLATFORM_PUBLIC void setVisible(bool visible) noexcept;

		LUX_PLATFORM_PUBLIC bool isVisible() const noexcept;

		LUX_PLATFORM_PUBLIC int width() const noexcept;

		LUX_PLATFORM_PUBLIC int height() const noexcept;

	private:
		LUX_PLATFORM_PUBLIC virtual void paint();

		bool		_visible;
		std::string _title;
	};
}