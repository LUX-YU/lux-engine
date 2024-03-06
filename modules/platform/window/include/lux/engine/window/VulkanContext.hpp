#pragma once
#include "GraphicContext.hpp"
#include <cstdint>

namespace lux::window
{
    class VulkanContext : public GraphicContext
    {
    public:
        LUX_PLATFORM_PUBLIC VulkanContext();

        LUX_PLATFORM_PUBLIC ~VulkanContext() override;

        LUX_PLATFORM_PUBLIC bool acceptVisitor(ContextVisitor* visitor, int operation) override;

        LUX_PLATFORM_PUBLIC GraphicAPI apiType() const override;

        LUX_PLATFORM_PUBLIC bool apiInit() override;

        LUX_PLATFORM_PUBLIC void cleanUp() override;

    private:
        bool create_instance();

        bool        _init{false};
        void*       _instance;
        uint32_t    _extension_count;
    };
}
