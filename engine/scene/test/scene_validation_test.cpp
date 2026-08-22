#include <lux/engine/scene/SceneAssetSerDeser.hpp>

#include <cassert>
#include <span>
#include <string_view>

namespace
{
    [[nodiscard]] uuids::uuid uuid(std::string_view text)
    {
        const auto parsed = uuids::uuid::from_string(text);
        assert(parsed);
        return *parsed;
    }

    [[nodiscard]] lux::ecs::scene_format::SectionRecord section(
        std::string_view id,
        std::string path)
    {
        lux::ecs::scene_format::SectionRecord result;
        result.id = lux::ecs::scene_format::EntitySectionId{uuid(id)};
        result.source = lux::ecs::scene_format::StoredSectionSource{std::move(path)};
        result.content_digest = lux::cxx::algorithm::Sha256::hash(
            std::span<const std::byte>{});
        result.compression = lux::ecs::scene_format::SectionCompression::NONE;
        result.encoded_bytes = 64u;
        result.decoded_bytes = 64u;
        result.entity_count = 1u;
        return result;
    }

    [[nodiscard]] lux::scene::SceneDescription validPackage()
    {
        lux::scene::SceneDescription result;
        result.id = lux::asset::asset_id_t{
            uuid("11111111-2222-4333-8444-555555555555")};
        result.spatial3d_catalog = {std::byte{0x2a}};
        result.sections.push_back(section(
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "/Game/startup_lxes"));
        result.startup_sections.push_back(result.sections.front().id);
        result.required_extensions.push_back(lux::scene::RequiredExtension{
            lux::extensions::ExtensionId{"org.lux.test.extension"},
            1u,
            0u});
        result.required_components.push_back(
            lux::ecs::scene_format::RequiredComponentSchema{
                lux::ecs::componentSchemaId("org.lux.test.component"),
                1u});
        return result;
    }
}

int main()
{
    namespace scene = lux::scene;

    const auto package = validPackage();
    assert(scene::validateSceneDescription(package));
    assert(scene::validateSectionRecord(package.sections.front()));

    auto invalid_path = package;
    std::get<lux::ecs::scene_format::StoredSectionSource>(
        invalid_path.sections.front().source).content_path =
            "Game/not-absolute";
    const auto rejected_path = scene::validateSceneDescription(invalid_path);
    assert(!rejected_path);
    assert(rejected_path.error().error ==
        scene::ESceneCodecError::INVALID_ARGUMENT);

    auto missing_digest = package;
    missing_digest.sections.front().content_digest = {};
    const auto rejected_digest =
        scene::validateSceneDescription(missing_digest);
    assert(!rejected_digest);
    assert(rejected_digest.error().error ==
        scene::ESceneCodecError::DIGEST_MISMATCH);

    auto missing_startup = package;
    missing_startup.startup_sections.front() =
        lux::ecs::scene_format::EntitySectionId{
            uuid("ffffffff-ffff-4fff-8fff-ffffffffffff")};
    const auto rejected_startup =
        scene::validateSceneDescription(missing_startup);
    assert(!rejected_startup);
    assert(rejected_startup.error().error ==
        scene::ESceneCodecError::INVALID_REFERENCE);

    auto cycle = package;
    cycle.sections.push_back(section(
        "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff",
        "/Game/dependent_lxes"));
    cycle.sections[0].dependencies.push_back(cycle.sections[1].id);
    cycle.sections[1].dependencies.push_back(cycle.sections[0].id);
    const auto rejected_cycle = scene::validateSceneDescription(cycle);
    assert(!rejected_cycle);
    assert(rejected_cycle.error().error ==
        scene::ESceneCodecError::INVALID_REFERENCE);

    return 0;
}
