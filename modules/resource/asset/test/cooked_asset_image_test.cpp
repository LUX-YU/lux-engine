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
        std::array<std::uint8_t, 16U> id{};
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

    struct AuxiliaryHeader final
    {
        std::uint64_t tag{};
        std::uint64_t size{};
    };

    static_assert(sizeof(OldAssetHeaderV1) == 72U);
    static_assert(sizeof(AuxiliaryHeader) == 16U);
    static_assert(std::is_trivially_copyable_v<OldAssetHeaderV1>);
}

int main()
{
    using namespace lux::asset;
    constexpr AssetDecodeLimits generous_limits{1024U * 1024U, 1024U * 1024U, 4U};
    constexpr std::array info{std::byte{0x11}, std::byte{0x22}};
    constexpr std::array data{std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}};
    constexpr std::array auxiliary_bytes{std::byte{0x41}, std::byte{0x42}};
    constexpr AuxiliaryHeader auxiliary{0x1020304050607080ULL, auxiliary_bytes.size()};

    OldAssetHeaderV1 header{};
    header.magic = 0x01309143U;
    header.version = kCookedAssetVersionV1;
    header.info_offset = sizeof(header);
    header.info_size = info.size();
    header.data_offset = header.info_offset + header.info_size;
    header.data_size = data.size();
    header.metadata.id[0] = 0x51U;
    header.metadata.id[15] = 0x09U;
    header.metadata.type = 0U;
    header.metadata.date = 0x0102030405060708ULL;
    const AssetId requested{header.metadata.id};

    std::vector<std::byte> fixture(
        sizeof(header) + info.size() + data.size() + sizeof(auxiliary) + auxiliary_bytes.size()
    );
    std::size_t offset{};
    std::memcpy(fixture.data() + offset, &header, sizeof(header));
    offset += sizeof(header);
    std::memcpy(fixture.data() + offset, info.data(), info.size());
    offset += info.size();
    std::memcpy(fixture.data() + offset, data.data(), data.size());
    offset += data.size();
    std::memcpy(fixture.data() + offset, &auxiliary, sizeof(auxiliary));
    offset += sizeof(auxiliary);
    std::memcpy(fixture.data() + offset, auxiliary_bytes.data(), auxiliary_bytes.size());

    auto owner = lux::cxx::SharedBytes<>::copyOf(fixture);
    const auto decoded = inspectCookedAssetImage(requested, owner, generous_limits);
    assert(decoded);
    assert(decoded->magic() == header.magic);
    assert(decoded->version() == kCookedAssetVersionV1);
    assert(decoded->metadata().id == requested);
    assert(decoded->metadata().date == header.metadata.date);
    assert(decoded->information().view().size() == info.size());
    assert(decoded->data().view().size() == data.size());
    assert(decoded->auxiliaryPayloads().size() == 1U);
    assert(decoded->auxiliaryPayloads()[0].tag == auxiliary.tag);
    assert(decoded->auxiliaryPayloads()[0].bytes.view().size() == auxiliary_bytes.size());
    assert(decoded->data().use_count() > 1L);

    auto truncated = fixture;
    truncated.pop_back();
    auto truncated_result = inspectCookedAssetImage(
        requested,
        lux::cxx::SharedBytes<>::copyOf(truncated),
        generous_limits
    );
    assert(!truncated_result);
    assert(truncated_result.error().code == EAssetDecodeError::INVALID_LAYOUT);

    auto corrupt = fixture;
    const std::uint64_t impossible = corrupt.size() + 1U;
    std::memcpy(corrupt.data() + offsetof(OldAssetHeaderV1, data_offset), &impossible, sizeof(impossible));
    assert(!inspectCookedAssetImage(
        requested,
        lux::cxx::SharedBytes<>::copyOf(corrupt),
        generous_limits
    ));

    auto mismatch_bytes = header.metadata.id;
    mismatch_bytes[15] ^= 0x01U;
    const auto mismatch = inspectCookedAssetImage(
        AssetId{mismatch_bytes},
        lux::cxx::SharedBytes<>::copyOf(fixture),
        generous_limits
    );
    assert(!mismatch && mismatch.error().code == EAssetDecodeError::ASSET_ID_MISMATCH);

    const auto limited = inspectCookedAssetImage(
        requested,
        lux::cxx::SharedBytes<>::copyOf(fixture),
        AssetDecodeLimits{fixture.size() - 1U, fixture.size(), 4U}
    );
    assert(!limited && limited.error().code == EAssetDecodeError::LIMIT_EXCEEDED);

    auto duplicate = fixture;
    duplicate.insert(
        duplicate.end(),
        reinterpret_cast<const std::byte*>(&auxiliary),
        reinterpret_cast<const std::byte*>(&auxiliary) + sizeof(auxiliary)
    );
    duplicate.insert(duplicate.end(), auxiliary_bytes.begin(), auxiliary_bytes.end());
    const auto duplicate_result = inspectCookedAssetImage(
        requested,
        lux::cxx::SharedBytes<>::copyOf(duplicate),
        AssetDecodeLimits{duplicate.size(), duplicate.size(), 4U}
    );
    assert(!duplicate_result && duplicate_result.error().code == EAssetDecodeError::INVALID_LAYOUT);
    return 0;
}
