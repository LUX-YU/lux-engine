#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <cstdio>
#include <limits>
#include <string_view>
#include <type_traits>

#define CHECK(condition) \
    do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

using namespace lux::render;
namespace cxx = lux::cxx;
namespace rdesc = lux::rdesc;

static_assert(std::is_same_v<SceneId, cxx::SlotKey<SceneTag>>);
static_assert(std::is_same_v<ViewId, cxx::SlotKey<ViewTag>>);
static_assert(std::is_same_v<TargetId, cxx::SlotKey<TargetTag>>);
static_assert(std::is_same_v<EffectId, cxx::SlotKey<EffectTag>>);
static_assert(std::is_same_v<FeatureId, cxx::SlotKey<FeatureTag>>);
static_assert(!std::is_convertible_v<FeatureId, EffectId>);
static_assert(!std::is_convertible_v<SceneId, ViewId>);
static_assert(!std::is_convertible_v<ViewId, TargetId>);
static_assert(!std::is_convertible_v<TargetId, EffectId>);
static_assert(!std::is_constructible_v<SceneId, EffectId>);
static_assert(std::is_same_v<RenderError, lux::error::Error>);
static_assert(noexcept(TargetDesc{}.validate()));

template <typename Tag>
int lifecycle()
{
    cxx::SlotMap<int, Tag> values;
    CHECK(!values.find(cxx::SlotKey<Tag>{}));
    const auto first = values.insert(11);
    const auto survivor = values.insert(22);
    CHECK(first.isValid());
    CHECK(values.erase(first));
    CHECK(!values.find(first));
    CHECK(!values.erase(first));
    CHECK(*values.find(survivor) == 22);
    const auto replacement = values.insert(33);
    CHECK(replacement.index == first.index);
    CHECK(replacement.gen != first.gen);
    CHECK(*values.find(replacement) == 33);
    values.clear();
    CHECK(!values.find(survivor));
    CHECK(!values.find(replacement));
    const auto after_clear = values.insert(44);
    CHECK(*values.find(after_clear) == 44);
    CHECK(!values.find(first) && !values.find(survivor) && !values.find(replacement));
    return 0;
}

int identities()
{
    CHECK(lifecycle<SceneTag>() == 0);
    CHECK(lifecycle<ViewTag>() == 0);
    CHECK(lifecycle<TargetTag>() == 0);
    CHECK(lifecycle<EffectTag>() == 0);
    CHECK(lifecycle<FeatureTag>() == 0);
    return 0;
}

int scopes()
{
    struct SceneFixture
    {
        cxx::SlotMap<int, ViewTag> views;
    };
    cxx::SlotMap<SceneFixture, SceneTag> scenes;
    const auto scene_a = scenes.emplace();
    const auto scene_b = scenes.emplace();
    const auto view_a = scenes.find(scene_a)->views.insert(101);
    const auto view_b = scenes.find(scene_b)->views.insert(202);
    CHECK(view_a == view_b);
    CHECK(*scenes.find(scene_a)->views.find(view_a) == 101);
    CHECK(*scenes.find(scene_b)->views.find(view_b) == 202);
    CHECK(scenes.erase(scene_a));
    const auto rebuilt = scenes.emplace();
    const auto rebuilt_view = scenes.find(rebuilt)->views.insert(303);
    CHECK(!scenes.find(scene_a));
    CHECK(rebuilt != scene_a);
    CHECK(rebuilt_view == view_a); // The enclosing SceneId is necessary.
    CHECK(*scenes.find(scene_b)->views.find(view_b) == 202);

    // A fresh owner's first key can equal a previous owner's key. Core does not
    // implement endpoint admission or promise to recognize that provenance.
    cxx::SlotMap<int, SceneTag> endpoint_a;
    cxx::SlotMap<int, SceneTag> endpoint_b;
    CHECK(endpoint_a.insert(1) == endpoint_b.insert(2));
    return 0;
}

int exhaustion()
{
    // Exercise the real container's wraparound policy with a small generation.
    cxx::SlotMap<int, SceneTag, std::uint32_t, std::uint8_t> values;
    const auto original = values.insert(1);
    auto current = original;
    for (unsigned i = 0; i < 254; ++i)
    {
        CHECK(values.erase(current));
        CHECK(!values.find(original));
        current = values.insert(2);
        CHECK(!values.find(original));
    }
    CHECK(current.index != original.index);
    values.clear();
    CHECK(!values.find(current));
    return 0;
}

TargetDesc validTarget()
{
    return TargetDesc{640, 480, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
}

int targetValues()
{
    auto desc = validTarget();
    constexpr rdesc::ETextureFormat color_formats[]{
        rdesc::ETextureFormat::RGBA8_UNORM, rdesc::ETextureFormat::BGRA8_SRGB, rdesc::ETextureFormat::RGBA16_SFLOAT
    };
    for (const auto kind : {ETargetKind::OFFSCREEN, ETargetKind::SURFACE})
    {
        desc.kind = kind;
        for (const auto format : color_formats)
        {
            desc.format = format;
            for (const std::uint32_t samples : {1u, 2u, 4u, 8u, 16u, 32u, 64u})
            {
                desc.samples = samples;
                CHECK(desc.validate());
            }
        }
    }
    desc.width = (std::numeric_limits<std::uint32_t>::max)();
    CHECK(desc.validate()); // Actual device limits are deliberately not Core's contract.

    auto expectError = [](TargetDesc input, RenderError expected)
    {
        const auto result = input.validate();
        return !result && result.error() == expected;
    };
    desc = validTarget();
    desc.width = 0;
    CHECK(expectError(desc, RenderError{kInvalidTargetExtent, {0, 480, 0}}));
    desc = validTarget();
    desc.height = 0;
    CHECK(expectError(desc, RenderError{kInvalidTargetExtent, {640, 0, 0}}));
    desc = validTarget();
    constexpr rdesc::ETextureFormat invalid_formats[]{
        rdesc::ETextureFormat::UNDEFINED, rdesc::ETextureFormat::D32_SFLOAT,
        rdesc::ETextureFormat::BC7_UNORM, static_cast<rdesc::ETextureFormat>(-1)
    };
    for (const auto format : invalid_formats)
    {
        desc.format = format;
        CHECK(expectError(desc, RenderError{kInvalidTargetFormat, {static_cast<std::uint64_t>(format), 0, 0}}));
    }
    desc = validTarget();
    for (const std::uint32_t samples : {0u, 3u, 63u, 65u, 128u, 0xffffffffu})
    {
        desc.samples = samples;
        CHECK(expectError(desc, RenderError{kInvalidTargetSamples, {samples, 0, 0}}));
    }
    desc = validTarget();
    desc.kind = static_cast<ETargetKind>(255);
    CHECK(expectError(desc, RenderError{kInvalidTargetKind, {255, 0, 0}}));
    return 0;
}

int epochs()
{
    struct TargetFixture
    {
        TargetDesc desc;
        std::uint64_t target_epoch{1};
    };
    cxx::SlotMap<TargetFixture, TargetTag> targets;
    const auto id = targets.insert(TargetFixture{validTarget()});
    auto* value = targets.find(id);
    value->desc.width = 1280;
    ++value->target_epoch;
    CHECK(targets.find(id)->target_epoch == 2);
    CHECK(targets.find(id)->desc.width == 1280);
    CHECK(targets.erase(id));
    const auto replacement = targets.insert(TargetFixture{validTarget()});
    CHECK(replacement.index == id.index && replacement.gen != id.gen);
    CHECK(!targets.find(id));
    CHECK(targets.find(replacement)->target_epoch == 1);
    return 0;
}

int errors()
{
    const auto descriptors = renderCoreErrorDescriptors();
    CHECK(descriptors.size() == 5);
    const lux::error::ErrorId expected[]{
        kInvalidTargetExtent, kInvalidTargetFormat, kInvalidTargetSamples, kInvalidTargetKind, kInvalidCamera
    };
    for (std::size_t i = 0; i < descriptors.size(); ++i)
    {
        CHECK(lux::error::errorId(descriptors[i].name) == expected[i]);
        CHECK(!descriptors[i].message.empty());
        for (std::size_t j = 0; j < i; ++j)
        {
            CHECK(expected[i] != expected[j]);
        }
    }
    CHECK(descriptors.data() == renderCoreErrorDescriptors().data());
    return 0;
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const std::string_view test = argv[1];
    if (test == "ids") return identities();
    if (test == "scopes") return scopes();
    if (test == "exhaustion") return exhaustion();
    if (test == "target") return targetValues();
    if (test == "epochs") return epochs();
    if (test == "errors") return errors();
    return 2;
}
