#include <lux/engine/render/vulkan/retirement/Retirement.hpp>

#ifdef LUX_FORBIDDEN_INCLUDE
#include LUX_FORBIDDEN_INCLUDE
#endif

int main()
{
    static_assert(!std::is_copy_constructible_v<lux::render::vulkan::Buffer>);
    static_assert(!std::is_default_constructible_v<lux::render::vulkan::VulkanDevice>);
    return 0;
}
