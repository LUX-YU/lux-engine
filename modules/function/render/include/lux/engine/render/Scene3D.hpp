#pragma once
#include "Scene.hpp"
#include "RenderableObject3D.hpp"

#include <vector>

namespace lux::render
{
	class LuxScene3D : public LuxScene
	{
	public:
		LUX_FUNCTION_PUBLIC LuxScene3D();

		LUX_FUNCTION_PUBLIC void render();

	private:
		std::vector<std::unique_ptr<RenderableObject3D>> _renderable_objects;
	};
}
