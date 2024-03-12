#include <lux/engine/ui/WindowUI.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_impl_glfw.h>
#include <imgui/imgui_impl_opengl3.h>
#include <glad/glad.h>

namespace lux::ui
{
	WindowUI<EGraphicAPI::OpenGL3>::WindowUI(int width, int height, std::string title)
		: LuxWindow(width, height, std::move(title), std::make_shared<lux::window::GLContext>(4, 5))
	{
		gladLoadGLLoader((GLADloadproc)getProcAddress);

		glEnable(GL_DEPTH_TEST);

		this->subscribeWindowSizeChangeCallback(
			[this](auto&, auto width, auto height)
			{
				this->window_size_change_callback(width, height);
			}
		);

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = NULL;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows
		ImGuiStyle& style = ImGui::GetStyle();
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}
		ImGui_ImplGlfw_InitForOpenGL(currentContext(), true);
		ImGui_ImplOpenGL3_Init("#version 330 core");
	}

	WindowUI<EGraphicAPI::OpenGL3>::~WindowUI()
	{
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();

		ImGui::DestroyContext();
	}

	void WindowUI<EGraphicAPI::OpenGL3>::newFrame()
	{
		// new frame
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		float current_frame = (float)timeAfterFirstInitialization();
		_delta_time = current_frame - _last_frame;
		_last_frame = current_frame;

		glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

		// subwindows draw

		for (auto& widget : widgets)
		{
			widget->paint();
		}

		// imgui draw
		ImGui::Render(); // This will call EndFrame
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		ImGuiIO& io = ImGui::GetIO();
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			GLFWwindow* backup_current_context = currentContext();
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
			makeContextCurrent(backup_current_context);
		}
	}

	float WindowUI<EGraphicAPI::OpenGL3>::updateTime()
	{
		return _delta_time;
	}

	void WindowUI<EGraphicAPI::OpenGL3>::addSubwindow(std::unique_ptr<Widget> widget)
	{
		widgets.push_back(std::move(widget));
	}

	void WindowUI<EGraphicAPI::OpenGL3>::window_size_change_callback(int width, int height)
	{
		glViewport(0, 0, width, height);
	}

	void* WindowUI<EGraphicAPI::OpenGL3>::uiContext()
	{
		return ImGui::GetCurrentContext();
	}
}