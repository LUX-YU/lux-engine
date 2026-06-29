#pragma once
#include <lux/engine/function/visibility_ui.h>
#include <lux/engine/ui/UIElement.hpp>
#include <imgui.h>

#include <functional>

namespace lux::ui
{
	/**
	 * @brief UIElement that displays a Vulkan texture inside a Panel.
	 *
	 * Use within a Panel's paint() override:
	 * @code
	 *   void myPanel::paint() override {
	 *       scene_view_.draw();
	 *   }
	 * @endcode
	 */
	class LUX_FUNCTION_UI_PUBLIC SceneViewElement : public UIElement
	{
	public:
		explicit SceneViewElement(std::string label = "SceneView")
			: UIElement(std::move(label)) {}

		void setTextureID(ImTextureID id) noexcept { texture_id_ = id; }
		[[nodiscard]] ImTextureID textureID() const noexcept { return texture_id_; }

		void setAspectRatio(float ratio) noexcept { aspect_ratio_ = ratio; }
		[[nodiscard]] float aspectRatio() const noexcept { return aspect_ratio_; }

		/// After draw(), returns the actual content region used.
		[[nodiscard]] ImVec2 contentSize() const noexcept { return content_size_; }

		/// Register a callback invoked when the content region size changes.
		using ResizeCallback = std::function<void(uint32_t width, uint32_t height)>;
		void setResizeCallback(ResizeCallback cb) { resize_cb_ = std::move(cb); }

		void draw() override
		{
			if (!isVisible()) return;

			const ImVec2 avail = ImGui::GetContentRegionAvail();
			if (avail.x <= 0.f || avail.y <= 0.f)
			{
				content_size_ = {0.f, 0.f};
				return;
			}
			ImVec2 display_size = avail;

			if (aspect_ratio_ > 0.f)
			{
				const float avail_aspect = avail.x / avail.y;
				if (avail_aspect > aspect_ratio_)
					display_size.x = avail.y * aspect_ratio_;
				else
					display_size.y = avail.x / aspect_ratio_;
			}

			content_size_ = display_size;
			if (display_size.x <= 0.f || display_size.y <= 0.f)
				return;

			const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
			uint32_t w  = static_cast<uint32_t>(display_size.x * fb_scale.x);
			uint32_t h  = static_cast<uint32_t>(display_size.y * fb_scale.y);
			uint32_t pw = static_cast<uint32_t>(prev_content_size_.x * fb_scale.x);
			uint32_t ph = static_cast<uint32_t>(prev_content_size_.y * fb_scale.y);
			if ((w != pw || h != ph) && w > 0 && h > 0)
			{
				prev_content_size_ = display_size;
				if (resize_cb_) resize_cb_(w, h);
			}

			if (texture_id_)
				ImGui::Image(texture_id_, display_size);
		}

	private:
		ImTextureID    texture_id_{0};
		float          aspect_ratio_{0.f};
		ImVec2         content_size_{0, 0};
		ImVec2         prev_content_size_{0, 0};
		ResizeCallback resize_cb_;
	};

} // namespace lux::ui
