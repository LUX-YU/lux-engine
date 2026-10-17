#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/data/Encoding.hpp>

#include <cstdio>

using namespace lux::render;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); return 1; } } while (false)

static_assert(RenderCommand<SetTexture> && RenderCommand<SetTextureRegion> &&
    RenderCommand<ReleaseTexture> && RenderCommand<QueryTexture>);
static_assert(!EncodableData<SetTexture::Request> && !EncodableData<SetTextureRegion::Request>);
static_assert(!EncodableData<TextureBinding>);
// The authoritative member list now belongs to the Native Handler; tested at its installation boundary.

int main()
{
    CHECK(validateCommand(commandDescriptor<SetTexture>()));
    CHECK(validateCommand(commandDescriptor<SetTextureRegion>()));
    CHECK(validateCommand(commandDescriptor<ReleaseTexture>()));
    CHECK(validateCommand(commandDescriptor<QueryTexture>()));
    CHECK(commandDescriptor<SetTexture>().request.type == lux::cxx::typeToken<TextureWrite>());
    CHECK(commandDescriptor<SetTextureRegion>().request.type == lux::cxx::typeToken<TextureRegionWrite>());
    CHECK(commandDescriptor<ReleaseTexture>().response.type == lux::cxx::typeToken<RenderResult<Ack>>());
    CHECK(commandDescriptor<ReleaseTexture>().request.type == lux::cxx::typeToken<TexturePublished>());
    CHECK(commandDescriptor<QueryTexture>().response.type == lux::cxx::typeToken<QueryTexture::Response>());

    int destroyed = 0;
    auto source = std::shared_ptr<std::vector<std::byte>>(
        new std::vector<std::byte>(32, std::byte{8}),
        [&destroyed](auto* value) noexcept { ++destroyed; delete value; }
    );
    source->reserve(4096);
    const auto backing_bytes = source->capacity();
    const auto* original = source->data();
    auto payload = RenderPayload::fromVector(source, 8, 4);
    CHECK(payload);
    SetTexture::Request set;
    set.source = std::move(*payload);
    CHECK(set.source.retainedBytes() == backing_bytes);
    CHECK(set.source.bytes().data() == original + 8 && set.source.bytes().size() == 4);
    source.reset();
    SetTextureRegion::Request region;
    region.source = std::move(set.source);
    CHECK(set.source.bytes().empty() && set.source.retainedBytes() == 0);
    CHECK(region.source.retainedBytes() == backing_bytes);
    CHECK(region.source.bytes().data() == original + 8 && destroyed == 0);
    region = {};
    CHECK(destroyed == 1);

    TextureBinding binding{{{7, 4}, {3, 2}}, 1};
    auto argument = ConfigValue::make(binding);
    CHECK(argument && argument->retainedBytes() == sizeof(binding));
    auto retained = *argument;
    *argument = {};
    const auto* decoded = retained.borrow<TextureBinding>();
    CHECK(decoded && decoded->texture == binding.texture && decoded->lod == binding.lod);
    CHECK(!retained.borrow<TexturePublished>());
    auto config = ConfigValue::make(TextureStorage::Config{});
    CHECK(config && config->type() == lux::cxx::typeToken<TextureStorage::Config>());
    return 0;
}
