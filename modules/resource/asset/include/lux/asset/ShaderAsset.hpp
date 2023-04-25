#pragma once
#include "LuxAsset.hpp"

namespace lux::asset
{
	enum class ShaderType
	{
		GLSL,
		HLSL	// High Level Shader Language
	};

	class ShaderAsset : public LuxExternalAsset
	{
	public:
		ShaderAsset(FilePath filepath, ShaderType type)
			: LuxExternalAsset(std::move(filepath))
		{
			_shader_type = type;
		}

		[[nodiscard]] ShaderType shader_type() const
		{
			return _shader_type;
		}

	private:
		ShaderType _shader_type;
	};
}

