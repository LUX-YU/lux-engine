#pragma once
#include "SceneObject.hpp"

namespace lux::render
{
	class RenderableObject : public SceneObject
	{
	public:
		LUX_FUNCTION_PUBLIC RenderableObject(LuxScene* scene);

		LUX_FUNCTION_PUBLIC virtual ~RenderableObject();
	};
}
