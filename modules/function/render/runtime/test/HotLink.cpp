#include <lux/engine/render/runtime/NativeAssembly.hpp>

int main()
{
    auto candidate = lux::render::RenderRuntimeBuilder::create({});
    return candidate ? 0 : 1;
}
