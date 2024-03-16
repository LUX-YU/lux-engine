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

	class Scene3DWidgetDefault
	{
	public:
		Scene3DWidgetDefault()
		{
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

		void draw()
		{
			_shader.use();
			scope_bind vao_bind(_vertex_array);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 3);
		}

	private:
		// shaders
		lux::gapi::opengl::ShaderProgram	_shader;
		lux::gapi::opengl::VertexShader		_vertex_shader;
		lux::gapi::opengl::FragmentShader	_fragment_shader;

		// vao and vbo
		lux::gapi::opengl::VertexArray		_vertex_array;
		lux::gapi::opengl::ArrayBuffer		_vertex_buffer;
	};

	Scene3DWidget::Scene3DWidget()
	: Widget("Scene") 
	{
		_background = { 0.45f, 0.55f, 0.60f, 0.10f };
	}

	void Scene3DWidget::paint()
	{
		ImGui::Begin(title().c_str());
		auto widget_size = ImGui::GetContentRegionAvail();
		rescale_and_draw_framebuffer(widget_size.x, widget_size.y);
		if (ImGui::BeginChild("SceneRender"))
		{
			ImGui::Image(reinterpret_cast<ImTextureID>(_texture.rawObject()), widget_size, ImVec2(0, 1), ImVec2(1, 0));
			ImGui::EndChild();
		}

		ImGui::End();
	}

	void Scene3DWidget::drawMethod()
	{
		static Scene3DWidgetDefault default_scene;
		default_scene.draw();
	}

	void Scene3DWidget::setBackGround(float r, float g, float b, float a)
	{
		_background = { r, g, b, a };
	}

	void Scene3DWidget::rescale_and_draw_framebuffer(float width, float height)
	{
		scope_bind _frame_buffer_bind(_frame_buffer);
		scope_bind _texture_bind(_texture);
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
		Texture2D::sTSetTexParameter<TexturePName::MIN_FILTER>(TexturePNameValueMinFilter::LINEAR);
		Texture2D::sTSetTexParameter<TexturePName::MAG_FILTER>(TexturePNameValueMagFilter::LINEAR);
		FrameBuffer::sAddAttachment(_texture, AttachmentType::COLOR0);

		scope_bind render_buffer_bind(_render_buffer);
		RenderBuffer::sStorage(ImageFormat::DEPTH24_STENCIL8, width, height);
		FrameBuffer::sAddAttachment(_render_buffer, AttachmentType::DEPTH_STENCIL);
	
		glViewport(0, 0, width, height);
		glClearColor(_background[0], _background[1], _background[2], _background[3]);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

		drawMethod();
	}
}