#pragma once
#include <lux/engine/function/visibility_ui.h>
#include <string>

namespace lux::ui
{
	class Panel;

	/**
	 * @brief Fine-grained UI component rendered inside a Panel.
	 *
	 * Unlike Panel (which corresponds to a full ImGui::Begin/End window),
	 * a UIElement is a composable building block drawn *within* a panel's
	 * paint() method. Concrete examples include scene viewports, property
	 * grids, buttons, and tree views.
	 *
	 * Subclass UIElement and override draw() to implement custom components.
	 */
	class LUX_FUNCTION_UI_PUBLIC UIElement
	{
	public:
		explicit UIElement(std::string label = {}) : label_(std::move(label)) {}
		virtual ~UIElement() = default;

		UIElement(const UIElement&) = delete;
		UIElement& operator=(const UIElement&) = delete;
		UIElement(UIElement&&) noexcept = default;
		UIElement& operator=(UIElement&&) noexcept = default;

		/// Render this element. Called from within an active ImGui window.
		virtual void draw() = 0;

		[[nodiscard]] const std::string& label() const noexcept { return label_; }
		void setLabel(std::string label) { label_ = std::move(label); }

		void setVisible(bool v) noexcept { visible_ = v; }
		[[nodiscard]] bool isVisible() const noexcept { return visible_; }

	private:
		std::string label_;
		bool visible_{true};
	};

} // namespace lux::ui
