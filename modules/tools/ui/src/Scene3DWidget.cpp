#include <lux/engine/ui/Scene3DWidget.hpp>
#include <imgui.h>

static const char* vertex_shader_code = R"*(
#version 330

layout (location = 0) in vec3 pos;

void main()
{
	gl_Position = vec4(0.9*pos.x, 0.9*pos.y, 0.5*pos.z, 1.0);
}
)*";

static const char* fragment_shader_code = R"*(
#version 330

out vec4 color;

void main()
{
	color = vec4(0.0, 1.0, 0.0, 1.0);
}
)*";

static GLfloat vertices[] = {
	-0.5f, -0.5f, 0.0f, // Left  
	 0.5f, -0.5f, 0.0f, // Right 
	 0.0f,  0.5f, 0.0f  // Top   
};

namespace lux::ui
{
	using namespace ::lux::gapi::opengl;

	Scene3DWidget::Scene3DWidget()
	: Widget("Scene") 
	{
		create_framebuffer();
		_vertex_shader.compile(vertex_shader_code);
		_fragment_shader.compile(fragment_shader_code);
		_shader.attachShaders(_vertex_shader, _fragment_shader);
		_shader.link();

		{
			scope_bind vao_bind(_vertex_array);
			scope_bind vbo_bind(_vertex_buffer);
			ArrayBuffer::sBufferData(sizeof(vertices), vertices, BufferDataUsage::STATIC_DRAW);

			glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(GLfloat), (GLvoid*)0);
			glEnableVertexAttribArray(0);
		}
	}

	void Scene3DWidget::paint()
	{
		{
			_shader.use();
			scope_bind fbo_bind(_frame_buffer);
			glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
			scope_bind vao_bind(_vertex_array);
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}

		ImGui::Begin(title().c_str());
		const float window_width = ImGui::GetContentRegionAvail().x;
		const float window_height = ImGui::GetContentRegionAvail().y;

		// rescale_framebuffer(window_width, window_height);

		ImVec2 pos = ImGui::GetCursorScreenPos();

		// and here we can add our created texture as image to ImGui
		// unfortunately we need to use the cast to void* or I didn't find another way tbh
		ImGui::GetWindowDrawList()->AddImage(
			(void*)_texture.rawObject(),
			ImVec2(pos.x, pos.y),
			ImVec2(pos.x + window_width, pos.y + window_height),
			ImVec2(0, 1),
			ImVec2(1, 0)
		);

		ImGui::End();
	}

	void Scene3DWidget::create_framebuffer()
	{
		scope_bind frame_buffer_bind(_frame_buffer);
		scope_bind texture_bind(_texture);

		TextureImage colorImage{
			.level			= 0,
			.internalformat = ImageFormat::RGB,
			.width			= 800,
			.height			= 600,
			.format			= ImageFormat::RGB,
			.type			= DataType::UNSIGNED_BYTE,
			.data			= nullptr,
		};
		Texture2D::sImage2D(colorImage);
		Texture2D::sTSetTexParameter<TexturePName::MIN_FILTER>(TexturePNameValueMinFilter::LINEAR);
		Texture2D::sTSetTexParameter<TexturePName::MAG_FILTER>(TexturePNameValueMagFilter::LINEAR);

		FrameBuffer::sAddAttachment(_texture, AttachmentType::COLOR0);

		scope_bind render_buffer_bind(_render_buffer);
		RenderBuffer::sStorage(ImageFormat::DEPTH24_STENCIL8, 800, 600);
		FrameBuffer::sAddAttachment(_render_buffer, AttachmentType::DEPTH_STENCIL);
	}

	void Scene3DWidget::rescale_framebuffer(float width, float height)
	{
		scope_bind bind(_texture);
		TextureImage colorImage{
			.level			= 0,
			.internalformat = ImageFormat::RGB,
			.width			= (GLsizei)width,
			.height			= (GLsizei)height,
			.format			= ImageFormat::RGB,
			.type			= DataType::UNSIGNED_BYTE,
			.data			= nullptr
		};
		
		Texture2D::sImage2D(colorImage);
		FrameBuffer::sAddAttachment(_texture, AttachmentType::COLOR0);

		scope_bind render_buffer_bind(_render_buffer);
		RenderBuffer::sStorage(ImageFormat::DEPTH24_STENCIL8, width, height);
		FrameBuffer::sAddAttachment(_render_buffer, AttachmentType::DEPTH_STENCIL);
	}
}