#include "../Fixtures.hpp"

#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native header leaked into Data
#endif

int main()
{
    using namespace lux::render;
    const fixture::Record input{7, {1, 2, 3}, 0.5f, true, 1};
    auto bytes = encodeWire(input, {}, {16, 0});
    if (!bytes) return 1;
    auto decoded = decodeWire<fixture::Record>({DataSchema<fixture::Record>::describe(), *bytes, {}}, {16, 0});
    if (!decoded || *decoded != input) return 2;
    if (renderDataErrorDescriptors().size() != 5) return 3;
    return 0;
}
