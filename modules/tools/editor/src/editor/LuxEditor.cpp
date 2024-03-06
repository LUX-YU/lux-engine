#include "lux/engine/editor/LuxEditor.hpp"
#include "lux/engine/window/GLContext.hpp"
#include "lux/engine/window/LuxWindow.hpp"
#include "lux/engine/window/Subwindow.hpp"

#include <imgui/imgui.h>

class AssetBrowser : public lux::window::Subwindow
{
public:
	AssetBrowser()
		: lux::window::Subwindow("AssetBrowser")
	{

	}

private:
	void paint() override
	{
		ImGui::Begin("AssetBrowser");
		ImGui::Text("Hello World");
		ImGui::End();
	}
};

int main(int argc, char* argv[])
{
	lux::window::InitParameter windows_parameter;
	windows_parameter.height 		= 1080;
	windows_parameter.width  		= 1920;
	windows_parameter.title  		= "LuxEditor";

	auto context = std::make_shared<lux::window::GLContext>(4, 5);

	lux::window::LuxWindow window(windows_parameter, context);

	window.addSubwindow(std::make_unique<AssetBrowser>());

	window.enableVsync(true);

	return window.exec();
}
