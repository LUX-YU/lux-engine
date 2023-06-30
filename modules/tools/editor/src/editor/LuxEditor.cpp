#include "lux/engine/editor/LuxEditor.hpp"
#include "lux/engine/window/GLContext.hpp"
#include "lux/engine/window/LuxWindow.hpp"

int main(int argc, char* argv[])
{
	lux::window::InitParameter windows_parameter;
	windows_parameter.height 		= 1080;
	windows_parameter.width  		= 1920;
	windows_parameter.title  		= "LuxEditor";

	auto context = std::make_shared<lux::window::GLContext>(4, 5);

	lux::window::LuxWindow window(windows_parameter, context);
	window.enableVsync(true);

	while (!window.shouldClose())
	{
		window.pollEvents();


		window.swapBuffer();
	}

	return 0;
}
