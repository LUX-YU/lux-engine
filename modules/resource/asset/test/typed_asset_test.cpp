#include <lux/engine/resource/asset/Asset.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    struct Payload final
    {
        std::uint32_t value{};
    };

    class TestAsset final : public lux::asset::TAsset<Payload>
    {
    public:
        inline static constexpr auto asset_type = lux::asset::AssetTypeId::fromName("lux.test.asset");

        TestAsset(
            lux::asset::AssetInfo info,
            std::shared_ptr<const Payload> data,
            std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
        ) noexcept
            : TAsset(std::move(info), std::move(data), std::move(auxiliary))
        {
        }
    };

    class OtherAsset final : public lux::asset::TAsset<Payload>
    {
    public:
        inline static constexpr auto asset_type = lux::asset::AssetTypeId::fromName("lux.test.other-asset");

        OtherAsset(lux::asset::AssetInfo info, std::shared_ptr<const Payload> data) noexcept
            : TAsset(std::move(info), std::move(data), {})
        {
        }
    };

    [[nodiscard]] lux::asset::AssetId id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = tail;
        return lux::asset::AssetId{bytes};
    }
}

int main()
{
    using namespace lux::asset;

    static_assert(!std::is_copy_constructible_v<Asset>);
    static_assert(!std::is_move_constructible_v<Asset>);
    static_assert(std::is_same_v<TestAsset::data_type, Payload>);

    const auto payload = std::make_shared<const Payload>(Payload{42U});
    constexpr std::array auxiliary_bytes{std::byte{0x12U}, std::byte{0x34U}};
    const auto auxiliary = lux::cxx::SharedBytes<>::copyOf(auxiliary_bytes);
    std::vector<AssetAuxiliaryPayload> payloads{
        AssetAuxiliaryPayload{0xABCDEFU, auxiliary}
    };
    TestAsset value(
        AssetInfo{id(7U), TestAsset::asset_type, 123U},
        payload,
        std::move(payloads)
    );

    const Asset& base = value;
    assert(base.id() == id(7U));
    assert(base.type() == TestAsset::asset_type);
    assert(base.info().date == 123U);
    assert(base.as<TestAsset>() == &value);
    assert(base.as<OtherAsset>() == nullptr);
    assert(value.data().value == 42U);
    assert(value.sharedData() == payload);
    assert(base.auxiliaryPayloads().size() == 1U);
    assert(base.auxiliaryPayload(0xABCDEFU).view().size() == auxiliary_bytes.size());
    assert(base.auxiliaryPayload(1U).empty());
    return 0;
}
