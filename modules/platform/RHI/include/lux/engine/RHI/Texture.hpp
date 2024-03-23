#pragma once
#include "Definition.hpp"
#include <cstdint>

namespace lux::engine::rhi
{
	enum class ETextureDimension
	{
		ONE_DIM,
		ONE_DIM_ARRAY,
		TWO_DIM,
		TWO_DIM_ARRAY,
		THREE_DIM,
		CUBE_MAP,
		CUBE_MAP_ARRAY,
		BUFFER
	};

	enum class EPixelFormat
	{
		RGBA8_UNORM,
		RGBA8_UNORM_SRGB,
		RG8_UNORM,
		RGB10A2_UNORM,
		RGBA16_FLOAT,
		// depth
		DEPTH24_UNORM_STENCIL8_UINT
	};

	struct Texture
	{
		ETextureDimension dimension;
		uint32_t		  width;
		uint32_t		  height;
		EPixelFormat	  format;
	};
}