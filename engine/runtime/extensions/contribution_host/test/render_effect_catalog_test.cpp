#include <lux/engine/runtime/extensions/RenderEffects.hpp>

#include <cassert>
#include <type_traits>
#include <utility>

namespace
{
    lux::render::Expected<lux::render::FeatureHandle> createEffect(
        void*,
        const void*,
        std::size_t)
    {
        return lux::render::FeatureHandle{};
    }

    lux::runtime::RenderEffectDescriptor makeDescriptor(
        std::string_view id)
    {
        lux::runtime::RenderEffectDescriptor descriptor;
        descriptor.id = lux::render::RenderEffectId{id};
        descriptor.display_name = "Test effect";
        descriptor.factory = lux::render::makeSimpleFactory(
            &createEffect,
            "TestEffect");
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.test.provider"};
        return descriptor;
    }
} // namespace

int main()
{
    static_assert(std::is_same_v<
        typename decltype(std::declval<
            lux::runtime::RenderEffectDescriptor>().required_scene_features)::
            value_type,
        lux::scene::SceneFeatureId>);

    lux::runtime::RenderEffectCatalog catalog;
    assert(catalog.add(makeDescriptor("org.lux.test.render.effect")));
    assert(catalog.find(
        lux::render::renderEffectId("org.lux.test.render.effect")) != nullptr);

    const auto duplicate = catalog.add(
        makeDescriptor("org.lux.test.render.effect"));
    assert(!duplicate);
    assert(duplicate.error() ==
        lux::runtime::ERenderEffectCatalogError::DUPLICATE_EFFECT);

    const auto invalid = catalog.add(
        makeDescriptor("Org.lux.test.render.invalid"));
    assert(!invalid);
    assert(invalid.error() ==
        lux::runtime::ERenderEffectCatalogError::INVALID_DESCRIPTOR);

    auto valid_dependency = makeDescriptor(
        "org.lux.test.render.effect.with-scene-feature");
    valid_dependency.required_scene_features.emplace_back(
        "org.lux.test.scene.feature");
    assert(catalog.add(std::move(valid_dependency)));

    auto invalid_dependency = makeDescriptor(
        "org.lux.test.render.effect.invalid-scene-feature");
    invalid_dependency.required_scene_features.emplace_back(
        "Org.lux.test.scene.invalid");
    const auto invalid_dependency_result =
        catalog.add(std::move(invalid_dependency));
    assert(!invalid_dependency_result);
    assert(invalid_dependency_result.error() ==
        lux::runtime::ERenderEffectCatalogError::
            INVALID_SCENE_FEATURE_DEPENDENCY);

    auto duplicate_dependency = makeDescriptor(
        "org.lux.test.render.effect.duplicate-scene-feature");
    duplicate_dependency.required_scene_features.emplace_back(
        "org.lux.test.scene.feature");
    duplicate_dependency.required_scene_features.emplace_back(
        "org.lux.test.scene.feature");
    const auto duplicate_dependency_result =
        catalog.add(std::move(duplicate_dependency));
    assert(!duplicate_dependency_result);
    assert(duplicate_dependency_result.error() ==
        lux::runtime::ERenderEffectCatalogError::
            DUPLICATE_SCENE_FEATURE_DEPENDENCY);
    return 0;
}
