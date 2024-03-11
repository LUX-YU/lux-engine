#include <lux/engine/window/GLContext.hpp>
#include <lux/engine/ui/WindowUI.hpp>
#include <imgui/imgui.h>

using namespace lux::ui;

class AssetBrowser : public Widget
{
public:
	AssetBrowser() : Widget("Asset Browser")
	{

	}

	void paint(void* imgui_context) override
	{
		ImGuiWindowFlags window_flags = 0;
		window_flags |= ImGuiWindowFlags_NoBackground;
		// window_flags |= ImGuiWindowFlags_NoTitleBar;
		static bool open_ptr = true;

		ImGui::Begin("Asset Browser", &open_ptr, window_flags);
		ImGui::Text("This is prototype of asset browser");
		ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
		ImGui::End();
	}
};

int main(int argc, char* argv[])
{
	lux::ui::WindowUI<EGraphicAPI::OpenGL3> window(1920, 1080);

	auto asset_brower = std::make_unique<AssetBrowser>();
	window.addSubwindow(std::move(asset_brower));
	window.enableVsync(true);

	return window.exec();
}
