#pragma once
#include <lux/engine/platform/visibility.h>

// TODO move to RHI module
namespace lux::window
{
    class LuxWindowImpl;
    class ContextVisitor;

    enum class EGraphicAPI
	{
		OpenGL3,
        EGL,
		Vulkan,
		DirectX11,
        DirectX12,
		Metal,
		Software
	};

    class GraphicContext
    {
    public:
        LUX_PLATFORM_PUBLIC virtual ~GraphicContext() = default;
        LUX_PLATFORM_PUBLIC virtual bool acceptVisitor(ContextVisitor* visitor, int operation) = 0;

        // invoke after window is been created
        LUX_PLATFORM_PUBLIC virtual bool apiInit() = 0;
        LUX_PLATFORM_PUBLIC virtual void cleanUp() = 0;
        LUX_PLATFORM_PUBLIC virtual EGraphicAPI apiType() const = 0;
    };
} // namespace lux::window
