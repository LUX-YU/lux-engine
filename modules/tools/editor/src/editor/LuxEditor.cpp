#include <lux/engine/window/GLContext.hpp>
#include <lux/engine/ui/WindowUI.hpp>
#include <lux/engine/ui/Scene3DWidget.hpp>
#include <imgui.h>

using namespace lux::ui;

class AssetBrowser : public Widget
{
public:
	AssetBrowser() : Widget("Asset Browser")
	{

	}

	void paint() override
	{
		ImGuiWindowFlags window_flags = 0;
		// window_flags |= ImGuiWindowFlags_NoBackground;
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
	lux::ui::WindowUI<EGraphicAPI::OpenGL3> window(1920, 1080, "lux_editor_test");

	window.addWidget(std::make_unique<AssetBrowser>());
	auto scene_widget = std::make_unique<Scene3DWidget>();
	scene_widget->setBackGround(164, 255, 164, 1.000);
	window.addWidget(std::move(scene_widget));
	window.enableVsync(true);

	return window.exec();
}
