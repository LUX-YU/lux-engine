#pragma once

namespace lux::engine::rhi
{
	enum class EGraphicAPIs
	{
		OPENGL3,
		EGL,
		DIRECTX12,
		VULKAN
	};

	enum class ShaderType
	{
		GLSL,
		HLSL
	};
}