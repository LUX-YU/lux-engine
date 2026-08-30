#include "embed_shader.hpp"

#include <cstddef>
#include <cstdint>

int main()
{
    static_assert(lux::toolchain::smoke::smoke_shader_spirv_size >= 20U);
    static_assert(lux::toolchain::smoke::smoke_shader_info_size != 0U);
    static_assert(lux::toolchain::smoke::smoke_shader_spirv[0] == 0x03U);
    return 0;
}
