#include "Golden.data.hpp"
#include "Golden.serialize.hpp"
#include <cstdlib>
#include <lux/cxx/serialization/json.hpp>

using namespace lux::render;
static_assert(WireData<WireOnly> && !WireData<CpuOnly>);

// Fixture-specific envelope and codec. Never a memcpy/native-layout wire ABI.
std::string encode(const WireOnly& value)
{
    return "v3:" + lux::cxx::ser::to_json(value);
}

RenderResult<WireOnly> decode(std::string_view bytes)
{
    if (!bytes.starts_with("v3:"))
    {
        return lux::cxx::unexpected(RenderError{kInvalidVersion});
    }
    auto value = lux::cxx::ser::from_json<WireOnly>(bytes.substr(3));
    if (!value)
    {
        return lux::cxx::unexpected(RenderError{kDefinitionMismatch});
    }
    return std::move(*value);
}

int main()
{
    auto bytes = encode({17, "non-native UTF-8 message"});
    auto result = decode(bytes);
    if (!result || result->sequence != 17 || result->message != "non-native UTF-8 message")
    {
        std::abort();
    }
    if (decode("v2:{}") || decode("v3:{broken") || decode("v3:{}"))
    {
        std::abort();
    }
}
