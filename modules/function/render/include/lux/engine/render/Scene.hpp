#pragma once
#include <cstdint>
#include <memory>
#include <lux/engine/meta/LuxObject.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::window
{
	class LuxWindow;
}

namespace lux::render
{

	// Scene acually is a ui element?
	class LuxScene : public lux::meta::TLuxObject<LuxScene>
	{
		friend class SceneObject;
	public:
		LUX_FUNCTION_PUBLIC LuxScene();

	private:
		LUX_FUNCTION_PUBLIC std::uint64_t assignId();

		class Impl;
		std::unique_ptr<Impl> _impl;
	};
}
