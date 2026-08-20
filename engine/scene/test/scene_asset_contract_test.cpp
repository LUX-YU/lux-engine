#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
    // Frozen LXSC v1 fixture emitted before resource/entity_scene retirement.
    constexpr std::array<std::byte, 310u> kLxscV1Golden{
        std::byte{0x4c}, std::byte{0x58}, std::byte{0x53}, std::byte{0x43},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x05}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x12}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x2f}, std::byte{0x47}, std::byte{0x61}, std::byte{0x6d},
        std::byte{0x65}, std::byte{0x2f}, std::byte{0x73}, std::byte{0x74},
        std::byte{0x61}, std::byte{0x72}, std::byte{0x74}, std::byte{0x75},
        std::byte{0x70}, std::byte{0x5f}, std::byte{0x6c}, std::byte{0x78},
        std::byte{0x65}, std::byte{0x73}, std::byte{0x16}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x6f}, std::byte{0x72},
        std::byte{0x67}, std::byte{0x2e}, std::byte{0x6c}, std::byte{0x75},
        std::byte{0x78}, std::byte{0x2e}, std::byte{0x74}, std::byte{0x65},
        std::byte{0x73}, std::byte{0x74}, std::byte{0x2e}, std::byte{0x63},
        std::byte{0x6f}, std::byte{0x6d}, std::byte{0x70}, std::byte{0x6f},
        std::byte{0x6e}, std::byte{0x65}, std::byte{0x6e}, std::byte{0x74},
        std::byte{0x16}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x6f}, std::byte{0x72}, std::byte{0x67}, std::byte{0x2e},
        std::byte{0x6c}, std::byte{0x75}, std::byte{0x78}, std::byte{0x2e},
        std::byte{0x74}, std::byte{0x65}, std::byte{0x73}, std::byte{0x74},
        std::byte{0x2e}, std::byte{0x65}, std::byte{0x78}, std::byte{0x74},
        std::byte{0x65}, std::byte{0x6e}, std::byte{0x73}, std::byte{0x69},
        std::byte{0x6f}, std::byte{0x6e}, std::byte{0x19}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x6f}, std::byte{0x72},
        std::byte{0x67}, std::byte{0x2e}, std::byte{0x6c}, std::byte{0x75},
        std::byte{0x78}, std::byte{0x2e}, std::byte{0x74}, std::byte{0x65},
        std::byte{0x73}, std::byte{0x74}, std::byte{0x2e}, std::byte{0x70},
        std::byte{0x72}, std::byte{0x65}, std::byte{0x73}, std::byte{0x65},
        std::byte{0x6e}, std::byte{0x74}, std::byte{0x61}, std::byte{0x74},
        std::byte{0x69}, std::byte{0x6f}, std::byte{0x6e}, std::byte{0x11},
        std::byte{0x11}, std::byte{0x11}, std::byte{0x11}, std::byte{0x22},
        std::byte{0x22}, std::byte{0x43}, std::byte{0x33}, std::byte{0x84},
        std::byte{0x44}, std::byte{0x55}, std::byte{0x55}, std::byte{0x55},
        std::byte{0x55}, std::byte{0x55}, std::byte{0x55}, std::byte{0x01},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xa2},
        std::byte{0xc1}, std::byte{0xdd}, std::byte{0x3f}, std::byte{0xb8},
        std::byte{0x29}, std::byte{0x72}, std::byte{0xb7}, std::byte{0x04},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x2a},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa},
        std::byte{0xbb}, std::byte{0xbb}, std::byte{0x4c}, std::byte{0xcc},
        std::byte{0x8d}, std::byte{0xdd}, std::byte{0xee}, std::byte{0xee},
        std::byte{0xee}, std::byte{0xee}, std::byte{0xee}, std::byte{0xee},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa},
        std::byte{0xbb}, std::byte{0xbb}, std::byte{0x4c}, std::byte{0xcc},
        std::byte{0x8d}, std::byte{0xdd}, std::byte{0xee}, std::byte{0xee},
        std::byte{0xee}, std::byte{0xee}, std::byte{0xee}, std::byte{0xee},
        std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0xe3}, std::byte{0xb0}, std::byte{0xc4},
        std::byte{0x42}, std::byte{0x98}, std::byte{0xfc}, std::byte{0x1c},
        std::byte{0x14}, std::byte{0x9a}, std::byte{0xfb}, std::byte{0xf4},
        std::byte{0xc8}, std::byte{0x99}, std::byte{0x6f}, std::byte{0xb9},
        std::byte{0x24}, std::byte{0x27}, std::byte{0xae}, std::byte{0x41},
        std::byte{0xe4}, std::byte{0x64}, std::byte{0x9b}, std::byte{0x93},
        std::byte{0x4c}, std::byte{0xa4}, std::byte{0x95}, std::byte{0x99},
        std::byte{0x1b}, std::byte{0x78}, std::byte{0x52}, std::byte{0xb8},
        std::byte{0x55}, std::byte{0x00}, std::byte{0x40}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x40}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x15}, std::byte{0x95},
        std::byte{0x37}, std::byte{0xf5}, std::byte{0x6f}, std::byte{0xb7},
        std::byte{0xaf}, std::byte{0x6b}, std::byte{0x02}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x8f}, std::byte{0xa6},
        std::byte{0x03}, std::byte{0xf1}, std::byte{0x9c}, std::byte{0x51},
        std::byte{0xd8}, std::byte{0x1d}, std::byte{0x03}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}};

    [[nodiscard]] uuids::uuid uuid(std::string_view text)
    {
        const auto parsed = uuids::uuid::from_string(text);
        assert(parsed);
        return *parsed;
    }

    [[nodiscard]] lux::scene::SceneDescription fixturePackage()
    {
        namespace format = lux::ecs::scene_format;
        namespace scene = lux::scene;

        const auto package_uuid =
            uuid("11111111-2222-4333-8444-555555555555");
        const auto section_uuid =
            uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");

        scene::SceneDescription package;
        package.id = package_uuid;
        package.features.push_back(scene::SceneFeatureRequest{
            scene::SceneFeatureId{"org.lux.test.presentation"},
            1u,
            {std::byte{0x2a}}});
        package.startup_sections.push_back(
            format::EntitySectionId{section_uuid});

        scene::SectionRecord section;
        section.id = format::EntitySectionId{section_uuid};
        section.source = scene::StoredSectionSource{"/Game/startup_lxes"};
        section.content_digest = lux::cxx::algorithm::Sha256::hash(
            std::span<const std::byte>{});
        section.compression = scene::SectionCompression::None;
        section.encoded_bytes = 64u;
        section.decoded_bytes = 64u;
        section.entity_count = 1u;
        section.required_components.push_back(scene::RequiredComponentSchema{
            lux::ecs::componentSchemaId("org.lux.test.component"),
            1u});
        package.sections.push_back(section);
        package.required_extensions.push_back(scene::RequiredExtension{
            lux::extensions::ExtensionId{"org.lux.test.extension"},
            1u,
            0u});
        return package;
    }
}

int main()
{
    namespace format = lux::ecs::scene_format;
    namespace scene = lux::scene;

    static_assert(!std::is_same_v<
        scene::SceneFeatureId,
        lux::extensions::ContributionId>);
    static_assert(!std::is_constructible_v<
        lux::asset::asset_id_t,
        format::EntitySectionId>);

    const auto package = fixturePackage();
    const auto encoded = scene::SceneAssetSerDeser::encodeData(
        package.id,
        package);
    assert(encoded);
    assert(encoded->size() ==
        sizeof(lux::asset::AssetFileHeader) + kLxscV1Golden.size());
    lux::asset::AssetFileHeader outer{};
    std::memcpy(&outer, encoded->data(), sizeof(outer));
    assert(outer.magic_number == scene::kSceneAssetMagic);
    assert(outer.info.id == package.id);
    assert(outer.info.type == scene::kSceneAssetType);
    assert(outer.data_size == kLxscV1Golden.size());
    assert(std::equal(
        encoded->begin() + sizeof(outer),
        encoded->end(),
        kLxscV1Golden.begin()));

    const auto decoded = scene::SceneAssetSerDeser::decodeData(kLxscV1Golden);
    assert(decoded && **decoded == package);
    const auto wrapped = scene::SceneAssetSerDeser::decodeData(*encoded);
    assert(wrapped && **wrapped == package);
    const auto reencoded = scene::SceneAssetSerDeser::encodeData(
        (*decoded)->id,
        **decoded);
    assert(reencoded && *reencoded == *encoded);

    const auto composed = scene::makeSceneAssetCodecCatalog(
        *lux::asset::runtimeAssetCodecCatalog());
    assert(composed);
    const auto* primary_descriptor = (*composed)->findByMagic(
        scene::kSceneAssetMagic);
    const auto* legacy_descriptor = (*composed)->findByMagic(
        scene::kSceneDescriptionMagic);
    assert(primary_descriptor != nullptr);
    assert(primary_descriptor == legacy_descriptor);
    assert(primary_descriptor->type == scene::kSceneAssetType);

    const auto legacy_shell = lux::asset::makeShellFromMemory(
        **composed,
        kLxscV1Golden.data(),
        kLxscV1Golden.size());
    assert(legacy_shell);
    assert((*legacy_shell)->info()->id == package.id);
    assert((*legacy_shell)->info()->type == scene::kSceneAssetType);
    assert((*legacy_shell)->as<scene::SceneAsset>() != nullptr);

    const auto manager = std::make_shared<lux::asset::AssetManager>(*composed);
    scene::SceneAssetSerDeser ser_deser{manager};
    const auto imported_legacy = ser_deser.fromLuxAssetMemory(
        kLxscV1Golden.data(),
        kLxscV1Golden.size());
    assert(imported_legacy);
    assert(imported_legacy->second == package.id);
    const auto upgraded_path =
        std::filesystem::temp_directory_path() /
        "lux_scene_asset_legacy_upgrade_contract.luxasset";
    std::error_code filesystem_error;
    std::filesystem::remove(upgraded_path, filesystem_error);
    assert(ser_deser.exportAsLuxAsset(package.id, upgraded_path) ==
        lux::asset::EAssetError::SUCCESS);
    std::ifstream upgraded_file{
        upgraded_path,
        std::ios::binary | std::ios::ate};
    assert(upgraded_file);
    const auto upgraded_size = upgraded_file.tellg();
    assert(upgraded_size >= 0);
    std::vector<std::byte> upgraded_image(
        static_cast<std::size_t>(upgraded_size));
    upgraded_file.seekg(0, std::ios::beg);
    upgraded_file.read(
        reinterpret_cast<char*>(upgraded_image.data()),
        static_cast<std::streamsize>(upgraded_image.size()));
    assert(upgraded_file);
    assert(upgraded_image == *encoded);
    upgraded_file.close();
    std::filesystem::remove(upgraded_path, filesystem_error);

    auto mismatched_outer = *encoded;
    lux::asset::AssetFileHeader mismatched_header{};
    std::memcpy(
        &mismatched_header,
        mismatched_outer.data(),
        sizeof(mismatched_header));
    mismatched_header.info.id = uuid(
        "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff");
    std::memcpy(
        mismatched_outer.data(),
        &mismatched_header,
        sizeof(mismatched_header));
    const auto mismatched_result =
        scene::SceneAssetSerDeser::decodeData(mismatched_outer);
    assert(!mismatched_result && mismatched_result.error().error ==
        scene::ESceneCodecError::OUTER_INNER_ID_MISMATCH);

    auto bad_magic = std::vector<std::byte>{
        kLxscV1Golden.begin(), kLxscV1Golden.end()};
    bad_magic[0] = std::byte{0u};
    const auto bad_magic_result =
        scene::SceneAssetSerDeser::decodeData(bad_magic);
    assert(!bad_magic_result && bad_magic_result.error().error ==
        scene::ESceneCodecError::BAD_MAGIC);

    auto bad_version = std::vector<std::byte>{
        kLxscV1Golden.begin(), kLxscV1Golden.end()};
    bad_version[4] = std::byte{2u};
    const auto bad_version_result =
        scene::SceneAssetSerDeser::decodeData(bad_version);
    assert(!bad_version_result && bad_version_result.error().error ==
        scene::ESceneCodecError::UNSUPPORTED_VERSION);

    const auto truncated = scene::SceneAssetSerDeser::decodeData(
        std::span<const std::byte>{kLxscV1Golden}.first(
            kLxscV1Golden.size() - 1u));
    assert(!truncated && truncated.error().error ==
        scene::ESceneCodecError::TRUNCATED);

    auto trailing = std::vector<std::byte>{
        kLxscV1Golden.begin(), kLxscV1Golden.end()};
    trailing.push_back(std::byte{0u});
    const auto trailing_result =
        scene::SceneAssetSerDeser::decodeData(trailing);
    assert(!trailing_result && trailing_result.error().error ==
        scene::ESceneCodecError::TRAILING_BYTES);

    scene::SceneCodecLimits tiny_limits;
    tiny_limits.maximum_names = 4u;
    const auto limited = scene::SceneAssetSerDeser::decodeData(
        kLxscV1Golden, tiny_limits);
    assert(!limited && limited.error().error ==
        scene::ESceneCodecError::LIMIT_EXCEEDED);

    auto bad_hash = std::vector<std::byte>{
        kLxscV1Golden.begin(), kLxscV1Golden.end()};
    bad_hash[135] ^= std::byte{1u};
    const auto bad_hash_result =
        scene::SceneAssetSerDeser::decodeData(bad_hash);
    assert(!bad_hash_result && bad_hash_result.error().error ==
        scene::ESceneCodecError::HASH_MISMATCH);

    auto duplicate_feature = package;
    duplicate_feature.features.push_back(package.features.front());
    const auto duplicate_result =
        scene::SceneAssetSerDeser::encodeData(
            duplicate_feature.id,
            duplicate_feature);
    assert(!duplicate_result && duplicate_result.error().error ==
        scene::ESceneCodecError::DUPLICATE_ID);

    auto missing_reference = package;
    missing_reference.startup_sections.front() =
        format::EntitySectionId{uuid(
            "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff")};
    const auto missing_result = scene::SceneAssetSerDeser::encodeData(
        missing_reference.id,
        missing_reference);
    assert(!missing_result && missing_result.error().error ==
        scene::ESceneCodecError::INVALID_REFERENCE);

    return 0;
}
