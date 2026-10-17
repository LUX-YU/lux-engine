#include <lux/engine/render/data/ShaderBlock.hpp>
#include <array>
#include <cstdlib>

using namespace lux::render;

static void check(bool condition) noexcept
{
    if (!condition) std::abort();
}

int main()
{
    ShaderBlock block;
    {
        std::string path = "transform";
        const std::array fields{lux::rdesc::DataGpuField{path, lux::rdesc::EDataScalar::FLOAT, 0, 4, 4, 0, 16}};
        block = ShaderBlock::from({lux::rdesc::EDataLayout::STD430, 64, 16, fields, {}, {}, 0});
    }
    auto copy = block;
    auto moved = std::move(block);
    check(copy == moved && moved.fields[0].path == "transform");
    copy.fields[0].matrix_stride = 8;
    check(copy != moved);
    copy = moved;
    copy.fields[0].offset = 16;
    check(copy != moved);
    copy = moved;
    copy.runtime_records = true;
    check(copy != moved);
    check(moved.size == 64 && moved.alignment == 16);
}
