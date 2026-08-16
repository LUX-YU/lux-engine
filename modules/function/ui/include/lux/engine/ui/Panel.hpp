#pragma once
#include <lux/engine/function/visibility_ui.h>
#include <string>
#include <memory>
#include <array>

#if !defined LUX_FUNCTION_UI_DLL_DISABLE
#include <imgui.h>
#endif

namespace lux::ui
{
	class UISystem;

	/**
	 * @brief A dockable sub-window (ImGui::Begin/End block) managed by UISystem.
	 *
	 * Each Panel corresponds to one ImGui window pane. The UISystem class drives
	 * its lifecycle: registration, per-frame painting, and deferred removal.
	 *
	 * Derive from Panel to create custom panes (scene viewports, inspectors,
	 * node editors, etc.).
	 */
	class LUX_FUNCTION_UI_PUBLIC Panel
	{
		friend class UISystem;
	public:
		Panel(std::string title, std::array<float, 2> suggest_size = {800, 600});
		virtual ~Panel();

		const std::string& title() const noexcept;

		void setVisible(bool visible) noexcept;

		bool isVisible() const noexcept;

		int width() const noexcept;

		int height() const noexcept;

#if !defined LUX_FUNCTION_UI_DLL_DISABLE
		static void setContext(void* context)
		{
			ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
		}
#endif

		size_t id() const { return id_; }

		const std::array<float, 2>& suggestSize() const noexcept
		{
			return suggest_size_;
		}

	private:
		void setID(size_t id) {
			id_ = id;
		}

		virtual void beforePaint() {}
		virtual void paint();
		virtual void afterPaint() {}

		size_t               id_;
		std::array<float, 2> suggest_size_;
		bool                 visible_;
		std::string          title_;
	};
}
