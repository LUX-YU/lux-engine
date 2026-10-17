#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/resource/Texture.hpp>

using namespace foundation_test;
using Declaration = lux::render::TextureStorage;

static unsigned created = 0, destroyed = 0;
struct Replacement final
{
    TextureStorageConfig config;
    ~Replacement() noexcept { ++destroyed; }
};

static RenderResult<std::unique_ptr<Replacement>> createReplacement(
    DeviceSession&, const TextureStorageConfig& config) noexcept
{
    ++created;
    return std::make_unique<Replacement>(config);
}

static bool compatible(const Replacement& value, const TextureStorageConfig& config) noexcept
{
    return value.config == config;
}

int main()
{
    Validation validation;
    auto bootstrap = instance(validation);
    DeviceOptions options;
    options.texture_descriptors = true;
    auto session = take(DeviceSession::create(bootstrap, options));
    auto& registry = session->resources();
    CHECK(registry.factory(Declaration::description.type) == nullptr && registry.size() == 0);
    static const Declaration::Ops ops{
        [](const void*, TextureId id) noexcept -> RenderResult<TexturePublished> { return TexturePublished{id, 991}; },
        [](void*, TextureId, std::uint64_t) noexcept -> RenderResult<void> { return {}; }
    };
    auto factory = makeStorageFactory<Declaration, Replacement, DeviceSession, createReplacement, compatible>(
        ops);
    CHECK(registry.installFactory(factory, TextureStorageConfig{}));
    CHECK(!registry.installFactory(factory, TextureStorageConfig{}));
    auto access = take(registry.getOrCreate<Declaration>(registry.root(), "explicit"));
    CHECK(created == 1 && take(access.query(TextureId{2, 3})).revision == 991);
    session.reset();
    CHECK(created == destroyed && validation.errors == 0);
}
