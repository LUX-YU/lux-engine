#pragma once
#include "Widget.hpp"
// #include <lux/engine/render/Scene3D.hpp>
#include <lux/engine/opengl3/glpp.hpp>

namespace lux::ui
{
	class Scene3DWidget : public Widget
	{
	public:
		Scene3DWidget();
		void paint() override;

		virtual void drawMethod();

		void setBackGround(float r, float g, float b, float a);

	private:
		void rescale_and_draw_framebuffer(float width, float height);

		// temporary, scene will use RHI and render api command list to draw later
		// lux::render::LuxScene3D		   _scene;
		
		// render buffer
		lux::gapi::opengl::Texture2D		_texture;
		lux::gapi::opengl::FrameBuffer		_frame_buffer;
		lux::gapi::opengl::RenderBuffer		_render_buffer;

		std::array<float, 4>				_background;
	};
}
