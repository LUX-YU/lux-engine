#include "External.data.hpp"
#include <array>

int main()
{
    using namespace lux::render;
    static_assert(CpuAuthoredGpuData<External>);
    constexpr auto layout = DataSchema<External>::gpuLayout();
    static_assert(layout.size == 48);
    std::array<std::byte, layout.size> bytes{};
    External value{{1, 2, 3}};
    auto packed = DataSchema<External>::pack(value, bytes);
    auto valid = validateDataSchema(DataSchema<External>::descriptor());
    return packed && valid ? 0 : 1;
}
