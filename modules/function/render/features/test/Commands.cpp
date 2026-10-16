#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/features/Texture.hpp>
#include <lux/engine/render/resource/Factory.hpp>

#include <cstdio>

using namespace lux::render;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); return 1; } } while (false)

static_assert(RenderCommand<SetTexture> && RenderCommand<SetTextureRegion> &&
    RenderCommand<ReleaseTexture> && RenderCommand<QueryTexture>);
static_assert(!WireData<SetTexture::Request> && !WireData<SetTextureRegion::Request>);
static_assert(WireData<TextureBinding>);
static_assert(std::tuple_size_v<TextureStorage::Commands> == 4);
static_assert(std::same_as<std::tuple_element_t<0, TextureStorage::Commands>, SetTexture>);
static_assert(std::same_as<std::tuple_element_t<1, TextureStorage::Commands>, SetTextureRegion>);
static_assert(std::same_as<std::tuple_element_t<2, TextureStorage::Commands>, ReleaseTexture>);
static_assert(std::same_as<std::tuple_element_t<3, TextureStorage::Commands>, QueryTexture>);

int main()
{
    CHECK(validateCommand(commandDescriptor<SetTexture>()));
    CHECK(validateCommand(commandDescriptor<SetTextureRegion>()));
    CHECK(validateCommand(commandDescriptor<ReleaseTexture>()));
    CHECK(validateCommand(commandDescriptor<QueryTexture>()));
    CHECK(commandDescriptor<SetTexture>().completion == ECompletionContract::RESOURCE_GPU_READY);
    CHECK(commandDescriptor<SetTextureRegion>().completion == ECompletionContract::RESOURCE_GPU_READY);
    CHECK(commandDescriptor<ReleaseTexture>().completion == ECompletionContract::STATE_COMMITTED);
    CHECK(commandDescriptor<ReleaseTexture>().response.type == lux::cxx::typeToken<Ack>());
    CHECK(commandDescriptor<QueryTexture>().completion == ECompletionContract::RESULT_AVAILABLE);

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
    CHECK(*CommandValueTraits<SetTexture::Request>::retainedBytes(set) == sizeof(set) + backing_bytes);
    CHECK(set.source.bytes().data() == original + 8 && set.source.bytes().size() == 4);
    source.reset();
    SetTextureRegion::Request region;
    region.source = std::move(set.source);
    CHECK(set.source.bytes().empty() && set.source.retainedBytes() == 0);
    CHECK(*CommandValueTraits<SetTextureRegion::Request>::retainedBytes(region) == sizeof(region) + backing_bytes);
    CHECK(region.source.bytes().data() == original + 8 && destroyed == 0);
    region = {};
    CHECK(destroyed == 1);

    TextureBinding binding{{3, 2}, 1};
    auto encoded = encodeWire(binding, {}, {12, 0});
    CHECK(encoded);
    auto decoded = decodeWire<TextureBinding>({DataSchema<TextureBinding>::describe(), *encoded, {}}, {12, 0});
    CHECK(decoded && decoded->texture == binding.texture && decoded->lod == binding.lod);
    CHECK(validateDataSchema(TextureStorage::config_schema));
    return 0;
}
