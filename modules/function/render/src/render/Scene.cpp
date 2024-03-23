#include <lux/engine/render/Scene.hpp>
#include <atomic>

namespace lux::render
{
	class LuxScene::Impl
	{
	public:
		static std::atomic<uint64_t> counter;
	};

	std::atomic<uint64_t> LuxScene::Impl::counter = 0;
	

	LuxScene::LuxScene()
		: _impl(std::make_unique<Impl>())
	{
		
	}

	std::uint64_t LuxScene::assignId()
	{
		return Impl::counter++;
	}
}
