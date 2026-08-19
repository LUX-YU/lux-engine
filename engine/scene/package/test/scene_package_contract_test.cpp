#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>
#include <lux/engine/scene/ScenePackageCodec.hpp>

#include <cassert>
#include <type_traits>

namespace
{
    [[nodiscard]] uuids::uuid uuid(std::string_view text)
    {
        const auto parsed = uuids::uuid::from_string(text);
        assert(parsed);
        return *parsed;
    }
}

int main()
{
    namespace legacy = lux::entity_scene;
    namespace format = lux::ecs::scene_format;
    namespace scene = lux::scene;

    static_assert(!std::is_same_v<
        scene::SceneFeatureId,
        lux::extensions::ContributionId>);

    const auto package_uuid =
        uuid("11111111-2222-4333-8444-555555555555");
    const auto section_uuid =
        uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");

    scene::ScenePackage package;
    package.id = scene::ScenePackageId{package_uuid};
    package.features.push_back(scene::SceneFeatureRequest{
        scene::SceneFeatureId{"org.lux.test.presentation"},
        1u,
        {std::byte{0x2a}}});
    package.startup_sections.push_back(format::EntitySectionId{section_uuid});

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

    const auto package_bytes = scene::encodeScenePackage(package);
    assert(package_bytes);
    const auto package_roundtrip = scene::decodeScenePackage(*package_bytes);
    assert(package_roundtrip && *package_roundtrip == package);

    legacy::EntitySceneManifest old;
    old.id = legacy::EntitySceneId{package_uuid};
    old.contributions.push_back(legacy::SceneContribution{
        lux::extensions::ContributionId{"org.lux.test.presentation"},
        1u,
        {std::byte{0x2a}}});
    old.startup_sections.push_back(legacy::EntitySectionId{section_uuid});

    legacy::EntitySectionRecord old_section;
    old_section.id = legacy::EntitySectionId{section_uuid};
    old_section.source = legacy::StoredSectionSource{"/Game/startup_lxes"};
    old_section.content_digest = section.content_digest;
    old_section.compression = legacy::EEntitySectionCompression::NONE;
    old_section.encoded_bytes = 64u;
    old_section.decoded_bytes = 64u;
    old_section.entity_count = 1u;
    old_section.required_components.push_back(
        legacy::RequiredComponentSchema{
            legacy::ComponentSchemaId{"org.lux.test.component"},
            1u});
    old.sections.push_back(old_section);
    old.required_extensions.push_back(legacy::RequiredExtension{
        lux::extensions::ExtensionId{"org.lux.test.extension"},
        1u,
        0u});

    const auto old_bytes = legacy::encodeEntitySceneManifest(old);
    assert(old_bytes);
    assert(*package_bytes == *old_bytes);

    const auto legacy_roundtrip = legacy::decodeEntitySceneManifest(
        *package_bytes);
    assert(legacy_roundtrip && *legacy_roundtrip == old);

    return 0;
}
