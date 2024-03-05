#include <lux/engine/render/Scene.hpp>

namespace lux::render
{
	class LuxScene::Impl
	{

	};

	LuxScene::LuxScene()
		: _impl(std::make_unique<Impl>())
	{
		
	}

	std::uint64_t assignId()
	{
		return 0;
	}
}