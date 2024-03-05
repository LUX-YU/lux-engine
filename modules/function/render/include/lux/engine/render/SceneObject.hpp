#pragma once
#include <lux/engine/function/visibility.h>
#include <lux/engine/meta/LuxObject.hpp>
#include <cstdint>

namespace lux::render
{
	class LuxScene;

	class SceneObject : public lux::meta::TLuxObject<SceneObject>
	{
	public:
		LUX_FUNCTION_PUBLIC SceneObject(LuxScene* scene);

		LUX_FUNCTION_PUBLIC virtual ~SceneObject() {};

		LUX_FUNCTION_PUBLIC std::uint64_t id() const;

	private:
		std::uint64_t _id;
		LuxScene* _scene;
	};
}