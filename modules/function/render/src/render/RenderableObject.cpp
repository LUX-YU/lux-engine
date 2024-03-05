#pragma once
#include <lux/engine/render/RenderableObject.hpp>
#include <lux/engine/render/Scene.hpp>

namespace lux::render
{
	RenderableObject::RenderableObject(LuxScene* scene)
	: SceneObject(scene){

	}

	RenderableObject::~RenderableObject()
	{

	}
}