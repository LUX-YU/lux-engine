#include "lux/editor/LuxEditor.hpp"
#include "lux/window/LuxWindow.hpp"

int main(int argc, char* argv[])
{
	lux::window::InitParameter windows_parameter;
	windows_parameter.graphic_api 	= lux::window::GraphicAPI::OPENGL;
	windows_parameter.height 		= 1080;
	windows_parameter.width  		= 1920;
	windows_parameter.title  		= "LuxEditor";

	lux::window::LuxWindow window(windows_parameter);
	window.enableVsync(true);

	while (!window.shouldClose())
	{
		window.pollEvents();


		window.swapBuffer();
	}

	return 0;
}
