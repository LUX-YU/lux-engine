#include <lux/engine/resource/asset/CookedAssetImage.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace
{
    struct OldAssetInfoV1 final
    {
        std::array<std::uint8_t, 16> id{};
        std::uint32_t type{};
        std::uint64_t date{};
    };

    struct OldAssetHeaderV1 final
    {
        std::uint32_t magic{};
        std::uint32_t version{};
        std::uint64_t info_offset{};
        std::uint64_t info_size{};
        std::uint64_t data_offset{};
        std::uint64_t data_size{};
        OldAssetInfoV1 metadata;
    };

    static_assert(sizeof(OldAssetHeaderV1) == 72u);
    static_assert(std::is_trivially_copyable_v<OldAssetHeaderV1>);
}

int main()
{
    using namespace lux::asset;
    constexpr CookedAssetImageLimits generous_limits{1024u * 1024u};
    constexpr std::array info{std::byte{0x11}, std::byte{0x22}};
    constexpr std::array data{
        std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}};
    OldAssetHeaderV1 header{};
    header.magic = 0x01309143u;
    header.version = kCookedAssetVersionV1;
    header.info_offset = sizeof(header);
    header.info_size = info.size();
    header.data_offset = header.info_offset + header.info_size;
    header.data_size = data.size();
    header.metadata.id[0] = 0x51u;
    header.metadata.id[15] = 0x09u;
    header.metadata.type = 0u;
    header.metadata.date = 0x0102030405060708ull;

    std::vector<std::byte> old_fixture(sizeof(header) + info.size() + data.size());
    std::memcpy(old_fixture.data(), &header, sizeof(header));
    std::memcpy(old_fixture.data() + sizeof(header), info.data(), info.size());
    std::memcpy(
        old_fixture.data() + sizeof(header) + info.size(),
        data.data(),
        data.size()
    );

    const auto decoded = inspectCookedAssetImage(old_fixture, generous_limits);
    assert(decoded);
    assert(decoded->magic == header.magic);
    assert(decoded->version == kCookedAssetVersionV1);
    assert(decoded->metadata.id == AssetId{header.metadata.id});
    assert(decoded->metadata.date == header.metadata.date);
    assert(decoded->info.size() == info.size());
    assert(decoded->data.size() == data.size());
    assert(decoded->auxiliary_payloads.empty());

    auto truncated = old_fixture;
    truncated.pop_back();
    assert(!inspectCookedAssetImage(truncated, generous_limits));

    auto corrupt = old_fixture;
    std::uint64_t impossible = corrupt.size() + 1u;
    std::memcpy(
        corrupt.data() + offsetof(OldAssetHeaderV1, data_offset),
        &impossible,
        sizeof(impossible)
    );
    assert(!inspectCookedAssetImage(corrupt, generous_limits));

    const auto limited = inspectCookedAssetImage(
        old_fixture,
        CookedAssetImageLimits{old_fixture.size() - 1u}
    );
    assert(!limited);
    assert(limited.error() == ECookedAssetImageError::LIMIT_EXCEEDED);
}
