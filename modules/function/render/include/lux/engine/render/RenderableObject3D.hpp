#pragma once
#include <lux/engine/function/visibility.h>
#include "RenderableObject.hpp"

namespace lux::render
{
	class LuxScene3D;

	class RenderableObject3D : public RenderableObject
	{
	public:
		LUX_FUNCTION_PUBLIC RenderableObject3D(LuxScene3D* _scene);

		LUX_FUNCTION_PUBLIC void visible(bool);

	protected:
		LUX_FUNCTION_PUBLIC LuxScene3D* scene();
	};
}
