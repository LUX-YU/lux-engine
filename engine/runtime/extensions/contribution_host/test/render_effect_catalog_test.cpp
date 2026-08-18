#include <lux/engine/runtime/extensions/RenderEffects.hpp>

#include <cassert>
#include <type_traits>

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
    static_assert(!std::is_same_v<
        lux::render::RenderEffectId,
        lux::extensions::ContributionId>);

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
    return 0;
}
