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

	private:
		void create_framebuffer();
		void rescale_framebuffer(float width, float height);

		// temporary, scene will use RHI and render api command list to draw later
		lux::gapi::opengl::Texture2D		_texture;
		lux::gapi::opengl::FrameBuffer		_frame_buffer;
		lux::gapi::opengl::ShaderProgram	_shader;
		lux::gapi::opengl::VertexShader		_vertex_shader;
		lux::gapi::opengl::FragmentShader	_fragment_shader;
		lux::gapi::opengl::VertexArray		_vertex_array;
		lux::gapi::opengl::ArrayBuffer		_vertex_buffer;
		lux::gapi::opengl::RenderBuffer		_render_buffer;
		// lux::render::LuxScene3D		   _scene;
		GLuint	RBO;
	};
}
