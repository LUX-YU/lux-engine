#include <lux/engine/render/resources/material/TextureSamplingRepresentationCatalog.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
    lux::cxx::expected<lux::render::TextureRefGPU, std::string>
    resolveChecker(std::uint32_t, std::uint32_t aux, std::uint32_t flags) noexcept
    {
        return lux::render::TextureRefGPU{1u, 0u, std::max(aux, 1u), flags};
    }
}

int
main()
{
    using namespace lux::render;

    auto production = builtinTextureSamplingRepresentationCatalog();
    assert(production.descriptors().size() == 1u);
    const auto* bindless = production.find(kBindlessTextureSamplingRepresentation);
    assert(bindless && bindless->representation_index == 0u);
    const auto bindless_reference = bindless->resolve_reference(123u, 7u, 9u);
    assert(
        bindless_reference && bindless_reference->representation_index == 0u &&
        bindless_reference->resource_index == 123u && bindless_reference->aux == 7u && bindless_reference->flags == 9u);

    auto descriptors = std::vector<TextureSamplingRepresentationDescriptor>{production.descriptors().front()};
    descriptors.push_back(
        {ownTextureSamplingRepresentationId(textureSamplingRepresentationId("test.render.texture.checker")),
         1u,
         &resolveChecker,
         {},
         "test.luxSampleTexture.checker"}
    );
    auto built = TextureSamplingRepresentationCatalog::build(std::move(descriptors));
    assert(built);
    const auto* checker = built->find(1u);
    assert(checker && checker->semantic_bindings.empty());
    const auto checker_reference = checker->resolve_reference(999u, 0u, 0x00ff8040u);
    assert(
        checker_reference && checker_reference->representation_index == 1u && checker_reference->resource_index == 0u &&
        checker_reference->aux == 1u && checker_reference->flags == 0x00ff8040u);

    auto invalid = TextureSamplingRepresentationCatalog::build({TextureSamplingRepresentationDescriptor{
        ownTextureSamplingRepresentationId(textureSamplingRepresentationId("not-canonical")),
        0u,
        &resolveChecker,
        {},
        "test.invalid"}}
    );
    assert(!invalid && invalid.error() == ETextureSamplingCatalogError::INVALID_ID);

    auto duplicate = TextureSamplingRepresentationCatalog::build(
        {TextureSamplingRepresentationDescriptor{
             ownTextureSamplingRepresentationId(textureSamplingRepresentationId("test.render.texture.duplicate")),
             0u,
             &resolveChecker,
             {},
             "test.duplicate.0"},
         TextureSamplingRepresentationDescriptor{
             ownTextureSamplingRepresentationId(textureSamplingRepresentationId("test.render.texture.duplicate")),
             1u,
             &resolveChecker,
             {},
             "test.duplicate.1"}}
    );
    assert(!duplicate && duplicate.error() == ETextureSamplingCatalogError::DUPLICATE_ID);

    static_assert(sizeof(TextureRefGPU) == 16u);
}
