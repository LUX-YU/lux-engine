#pragma once

namespace lux::engine::rhi
{
	class Resource
	{
	public:
		virtual ~Resource() = default;
	};

	/* Buffer related */
	enum class EBufferUsage
	{
		UNKNOWN,
		VERTEX,
		INDEX
	};

	struct BufferInfo
	{
		size_t			size;
		EBufferUsage	usage;
	};

	struct BufferCreateInfo
	{
		BufferInfo		info;
		void*			data;
	};

	class Buffer : public Resource
	{
	public:
		explicit Buffer(const BufferCreateInfo& create_info)
			: _info(create_info.info){}

		[[nodiscard]] size_t size() const
		{
			return _info.size;
		}

		[[nodiscard]] EBufferUsage usage() const
		{
			return _info.usage;
		}

	private:
		BufferInfo _info;
	};

	class UniformBuffer : public Buffer
	{
	};

	/* Shader related */
	class Shader : public Resource
	{
	public:
		
	};

	class GraphicsShader : public Shader
	{

	};

	class VertexShader : public GraphicsShader
	{

	};

	class PixelShader : public GraphicsShader
	{

	};

	class ComputerShader : public Shader
	{

	};
}