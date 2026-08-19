#include <lux/engine/scene/ScenePackageCodec.hpp>

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

    [[nodiscard]] lux::scene::SectionRecord section(
        std::string_view id,
        std::string path)
    {
        lux::scene::SectionRecord result;
        result.id = lux::ecs::scene_format::EntitySectionId{uuid(id)};
        result.source = lux::scene::StoredSectionSource{std::move(path)};
        result.content_digest = lux::cxx::algorithm::Sha256::hash(
            std::span<const std::byte>{});
        result.compression = lux::scene::SectionCompression::None;
        result.encoded_bytes = 64u;
        result.decoded_bytes = 64u;
        result.entity_count = 1u;
        return result;
    }

    [[nodiscard]] lux::scene::ScenePackage validPackage()
    {
        lux::scene::ScenePackage result;
        result.id = lux::scene::ScenePackageId{
            uuid("11111111-2222-4333-8444-555555555555")};
        result.features.push_back(lux::scene::SceneFeatureRequest{
            lux::scene::SceneFeatureId{"org.lux.test.presentation"},
            1u,
            {std::byte{0x2a}}});
        result.sections.push_back(section(
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "/Game/startup_lxes"));
        result.startup_sections.push_back(result.sections.front().id);
        result.required_extensions.push_back(lux::scene::RequiredExtension{
            lux::extensions::ExtensionId{"org.lux.test.extension"},
            1u,
            0u});
        result.required_components.push_back(
            lux::scene::RequiredComponentSchema{
                lux::ecs::componentSchemaId("org.lux.test.component"),
                1u});
        return result;
    }
}

int main()
{
    namespace scene = lux::scene;

    const auto package = validPackage();
    assert(scene::validateScenePackage(package));
    assert(scene::validateSectionRecord(package.sections.front()));

    auto invalid_path = package;
    std::get<scene::StoredSectionSource>(
        invalid_path.sections.front().source).content_path =
            "Game/not-absolute";
    const auto rejected_path = scene::validateScenePackage(invalid_path);
    assert(!rejected_path);
    assert(rejected_path.error().error ==
        scene::ScenePackageCodecError::InvalidArgument);

    auto duplicate_feature = package;
    duplicate_feature.features.push_back(
        duplicate_feature.features.front());
    const auto rejected_feature =
        scene::validateScenePackage(duplicate_feature);
    assert(!rejected_feature);
    assert(rejected_feature.error().error ==
        scene::ScenePackageCodecError::DuplicateId);

    auto missing_startup = package;
    missing_startup.startup_sections.front() =
        lux::ecs::scene_format::EntitySectionId{
            uuid("ffffffff-ffff-4fff-8fff-ffffffffffff")};
    const auto rejected_startup =
        scene::validateScenePackage(missing_startup);
    assert(!rejected_startup);
    assert(rejected_startup.error().error ==
        scene::ScenePackageCodecError::InvalidReference);

    auto cycle = package;
    cycle.sections.push_back(section(
        "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff",
        "/Game/dependent_lxes"));
    cycle.sections[0].dependencies.push_back(cycle.sections[1].id);
    cycle.sections[1].dependencies.push_back(cycle.sections[0].id);
    const auto rejected_cycle = scene::validateScenePackage(cycle);
    assert(!rejected_cycle);
    assert(rejected_cycle.error().error ==
        scene::ScenePackageCodecError::InvalidReference);

    return 0;
}
