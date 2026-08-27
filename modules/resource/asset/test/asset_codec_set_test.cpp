#include <lux/engine/resource/asset/AssetCodecSet.hpp>

#include <cassert>
#include <memory>

namespace
{
    struct PayloadA final
    {
        int value{};
    };

    struct PayloadB final
    {
        int value{};
    };

    lux::cxx::expected<lux::asset::DecodedAsset, lux::asset::EAssetCodecError>
    decode(std::span<const std::byte> input, const lux::asset::AssetDecodeContext& context) noexcept
    {
        if (input.size() > context.limits.max_input_bytes || sizeof(PayloadA) > context.limits.max_decoded_bytes)
        {
            return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
        }
        auto payload = std::make_shared<const PayloadA>(PayloadA{42});
        return lux::asset::DecodedAsset{payload, sizeof(PayloadA)};
    }

    lux::cxx::expected<std::vector<std::byte>, lux::asset::EAssetCodecError>
    encode(const void* payload, const lux::asset::AssetEncodeContext& context) noexcept
    {
        if (payload == nullptr || sizeof(PayloadA) > context.limits.max_encoded_bytes)
        {
            return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
        }
        return std::vector<std::byte>(sizeof(PayloadA));
    }
}

int
main()
{
    using namespace lux::asset;
    const auto type_a = AssetTypeId::fromName("lux.asset.test.a");
    const auto descriptor_a = AssetCodecDescriptor{
        type_a,
        "lux.asset.test.a",
        0x3141534cu,
        0u,
        lux::cxx::typeToken<PayloadA>(),
        &decode,
        &encode,
        {},
    };

    const auto base = AssetCodecSet::build({descriptor_a});
    assert(base);
    const auto copy = *base;
    assert(copy.find(type_a) != nullptr);
    assert(copy.findByMagic(0x3141534cu) != nullptr);
    assert(copy.findByPayloadType(lux::cxx::typeToken<PayloadA>()) != nullptr);
    const auto decoded = descriptor_a.decode({}, AssetDecodeContext{AssetCodecLimits{0u, sizeof(PayloadA), 0u}});
    assert(decoded);
    const auto encoded =
        descriptor_a.encode(decoded->payload.get(), AssetEncodeContext{AssetCodecLimits{0u, 0u, sizeof(PayloadA)}});
    assert(encoded);

    auto descriptor_b = descriptor_a;
    descriptor_b.type = AssetTypeId::fromName("lux.asset.test.b");
    descriptor_b.canonical_name = "lux.asset.test.b";
    descriptor_b.primary_magic = 0x3241534cu;
    descriptor_b.cpp_payload_type = lux::cxx::typeToken<PayloadB>();
    const auto extended = copy.extended(std::span{&descriptor_b, 1u});
    assert(extended);
    assert(copy.descriptors().size() == 1u);
    assert(extended->descriptors().size() == 2u);

    auto collision = descriptor_b;
    collision.type = type_a;
    collision.canonical_name = "lux.asset.test.collision";
    const auto rejected = AssetCodecSet::build({descriptor_a, collision});
    assert(!rejected);
    assert(rejected.error() == EAssetCodecError::INVALID_DESCRIPTOR);
}
