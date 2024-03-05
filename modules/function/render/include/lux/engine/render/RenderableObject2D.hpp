#pragma once
#include "RenderableObject.hpp"

namespace lux::render
{
	class LuxScene2D;

	class RenderableObject2D : public RenderableObject
	{
	public:
		LUX_FUNCTION_PUBLIC RenderableObject2D(LuxScene2D* _scene);

		LUX_FUNCTION_PUBLIC void visible(bool);

	private:
		LuxScene2D* _scene;
	};
}