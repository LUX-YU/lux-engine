#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/ecs/scene_format/PersistenceJournal.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace
{
    // Frozen fixtures emitted before resource/entity_scene retirement.
    constexpr std::string_view kLxesV1GoldenHex =
        "4c5845530100000002000000160000006f72672e6c75782e746573742e636f6d706f6e656e74aaaaaaaabbbb4ccc8dddeeeeeeeeeeee0100000001000000159537f56fb7af6b010000000100000001010000000100000000000000010000000000000000000000000000000000000000000000000000000000000000";
    constexpr std::string_view kPersistenceJournalV1GoldenHex =
        "4c58454a0100000085cdf882e49c5ba3180000006f72672e6c75782e74657374"
        "2e70657273697374656e63650100000060a5228bafb51d15ea4af76284ba5db6"
        "28d725e36884f26102c110d7569ecc7701000000000000000100000000000000"
        "684888c0ebb17f374298b65ee2807526c066094c701bcc7ebbe1c1095f494fc1"
        "2a99ebf9644df509155adcf4b938ea8ac0f5288d40c07680eaf98f9db162abae"
        "8a";

    [[nodiscard]] std::uint8_t hexDigit(char value)
    {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<std::uint8_t>(value - 'a' + 10);
        assert(false);
        return 0u;
    }

    [[nodiscard]] std::vector<std::byte> decodeHex(std::string_view hex)
    {
        assert(hex.size() % 2u == 0u);
        std::vector<std::byte> result;
        result.reserve(hex.size() / 2u);
        for (std::size_t index = 0u; index < hex.size(); index += 2u)
        {
            result.push_back(static_cast<std::byte>(
                (hexDigit(hex[index]) << 4u) |
                hexDigit(hex[index + 1u])));
        }
        return result;
    }

    [[nodiscard]] uuids::uuid uuid(std::string_view text)
    {
        const auto parsed = uuids::uuid::from_string(text);
        assert(parsed);
        return *parsed;
    }

    [[nodiscard]] lux::ecs::scene_format::EntitySectionImage fixtureImage()
    {
        namespace format = lux::ecs::scene_format;
        format::EntitySectionImage image;
        image.section = format::EntitySectionId{uuid(
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")};
        image.component_names.emplace_back();
        image.schemas.push_back(format::EntitySectionSchema{
            lux::ecs::componentSchemaId("org.lux.test.component"),
            1u,
            format::EEntityComponentStorage::TAG});
        image.archetypes.push_back(format::EntitySectionArchetype{{0u}});
        image.entities.push_back(
            format::EntitySectionEntity{0u, std::nullopt});
        return image;
    }
}

int main()
{
    namespace format = lux::ecs::scene_format;

    const auto lxes_golden = decodeHex(kLxesV1GoldenHex);
    const auto image = fixtureImage();
    const auto encoded = format::encodeEntitySectionImage(image);
    assert(encoded && *encoded == lxes_golden);
    const auto decoded = format::decodeEntitySectionImage(lxes_golden);
    assert(decoded && *decoded == image);
    const auto reencoded = format::encodeEntitySectionImage(*decoded);
    assert(reencoded && *reencoded == lxes_golden);

    const auto base_digest = format::entitySectionContentDigest(lxes_golden);
    const auto record = format::makePersistenceJournalRecord(
        format::PersistenceSchemaId{"org.lux.test.persistence"},
        1u,
        base_digest,
        1u,
        {std::byte{0x2a}});
    assert(record);
    const auto journal_golden = decodeHex(kPersistenceJournalV1GoldenHex);
    const auto journal = format::encodePersistenceJournalRecord(*record);
    assert(journal && *journal == journal_golden);
    const auto journal_decoded =
        format::decodePersistenceJournalRecord(journal_golden);
    assert(journal_decoded && *journal_decoded == *record);
    const auto journal_reencoded =
        format::encodePersistenceJournalRecord(*journal_decoded);
    assert(journal_reencoded && *journal_reencoded == journal_golden);

    auto bad_magic = lxes_golden;
    bad_magic[0] = std::byte{0u};
    const auto bad_magic_result = format::decodeEntitySectionImage(bad_magic);
    assert(!bad_magic_result && bad_magic_result.error().error ==
        format::EEntitySectionCodecError::BAD_MAGIC);

    auto bad_version = lxes_golden;
    bad_version[4] = std::byte{2u};
    const auto bad_version_result =
        format::decodeEntitySectionImage(bad_version);
    assert(!bad_version_result && bad_version_result.error().error ==
        format::EEntitySectionCodecError::UNSUPPORTED_VERSION);

    const auto truncated = format::decodeEntitySectionImage(
        std::span<const std::byte>{lxes_golden}.first(
            lxes_golden.size() - 1u));
    assert(!truncated && truncated.error().error ==
        format::EEntitySectionCodecError::TRUNCATED);

    auto trailing = lxes_golden;
    trailing.push_back(std::byte{0u});
    const auto trailing_result = format::decodeEntitySectionImage(trailing);
    assert(!trailing_result && trailing_result.error().error ==
        format::EEntitySectionCodecError::TRAILING_BYTES);

    auto bad_hash = lxes_golden;
    bad_hash[62] ^= std::byte{1u};
    const auto bad_hash_result = format::decodeEntitySectionImage(bad_hash);
    assert(!bad_hash_result && bad_hash_result.error().error ==
        format::EEntitySectionCodecError::HASH_MISMATCH);

    format::EntitySectionCodecLimits tiny_limits;
    tiny_limits.maximum_names = 1u;
    const auto limited = format::decodeEntitySectionImage(
        lxes_golden, tiny_limits);
    assert(!limited && limited.error().error ==
        format::EEntitySectionCodecError::LIMIT_EXCEEDED);

    auto corrupt_journal = journal_golden;
    corrupt_journal.back() ^= std::byte{1u};
    assert(!format::decodePersistenceJournalRecord(corrupt_journal));

    return 0;
}
