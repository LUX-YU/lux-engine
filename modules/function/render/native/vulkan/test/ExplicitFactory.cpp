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
        EGraphicsApi::VULKAN, ops);
    CHECK(registry.installFactory(factory));
    CHECK(!registry.installFactory(factory));
    CHECK(registry.configureType(Declaration::description, std::make_shared<const TextureStorageConfig>()));
    registry.freeze();
    auto access = take(registry.getOrCreate<Declaration>(registry.root(), "explicit"));
    CHECK(created == 1 && take(access.query(TextureId{2, 3})).revision == 991);
    session.reset();
    CHECK(created == destroyed && validation.errors == 0);
}
