#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <lux/engine/meta/TypeStaticInfo.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    struct PortableRecord final
    {
        std::uint32_t count{7U};
        std::string name;

        friend bool operator==(const PortableRecord&, const PortableRecord&) = default;
    };
}

template <>
struct lux::meta::TypeStaticInfo<PortableRecord>
{
    static constexpr bool available = true;
    static constexpr auto fields = std::tuple{
        lux::meta::typeStaticField<&PortableRecord::count>("count"),
        lux::meta::typeStaticField<&PortableRecord::name>("name")
    };
};

int main()
{
    using namespace lux;

    const auto codec = serialization::makePortableValueCodec<PortableRecord>();
    assert(codec.valid());
    assert(codec.type == cxx::typeToken<PortableRecord>());
    assert(!serialization::noPortableValueCodec().valid());

    std::vector<std::byte> default_bytes;
    assert(codec.encode_default(default_bytes));
    PortableRecord decoded_default{};
    decoded_default.count = 99U;
    decoded_default.name = "changed";
    assert(codec.decode(default_bytes, &decoded_default));
    assert(decoded_default == PortableRecord{});

    const PortableRecord source{42U, "portable"};
    std::vector<std::byte> encoded;
    assert(codec.encode(&source, encoded));
    PortableRecord decoded{};
    assert(codec.decode(encoded, &decoded));
    assert(decoded == source);

    auto trailing = encoded;
    trailing.push_back(std::byte{0x7f});
    const auto trailing_result = codec.decode(trailing, &decoded);
    assert(!trailing_result);
    assert(trailing_result.error().code == serialization::ESerializationError::INVALID_VALUE);

    assert(!codec.encode(nullptr, encoded));
    assert(!codec.decode(encoded, nullptr));
    return 0;
}
