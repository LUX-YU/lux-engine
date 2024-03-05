#include <lux/engine/render/SceneObject.hpp>

namespace lux::render
{
	SceneObject::SceneObject(LuxScene* scene)
	 : _scene(scene){

	}

	std::uint64_t SceneObject::id() const
	{
		return _id;
	}
}