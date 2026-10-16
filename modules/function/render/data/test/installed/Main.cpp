#include "../Fixtures.hpp"
#include <lux/engine/render/data/Payload.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>

#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native header leaked into Data
#endif

int main()
{
    using namespace lux::render;
    const fixture::Record input{7, {1, 2, 3}, 0.5f, true, 1};
    auto bytes = encodeData(input, {}, {16, 0});
    if (!bytes) return 1;
    auto decoded = decodeData<fixture::Record>({DataSchema<fixture::Record>::describe(), *bytes, {}}, {16, 0});
    if (!decoded || *decoded != input) return 2;
    if (renderDataErrorDescriptors().size() != 6) return 3;
    auto payload = RenderPayload::copyOf(bytes->view());
    if (payload.retainedBytes() != bytes->size() || payload.bytes().data() == bytes->data()) return 4;
    auto config = ConfigValue::make(std::uint32_t{42});
    if (!config || config->retainedBytes() != sizeof(std::uint32_t)) return 5;
    auto shared = *config;
    *config = {};
    if (!shared.borrow<std::uint32_t>() || *shared.borrow<std::uint32_t>() != 42) return 6;
    if (shared.borrow<float>() != nullptr) return 7;
    return 0;
}
