#include "Attachments.pass.hpp"
#include "HalfStorage.pass.hpp"
#include "Stages.pass.hpp"
#include "Storage.pass.hpp"
#include "Tonemap.pass.hpp"
#include <iostream>
#include <lux/engine/render/graph/Builder.hpp>

using namespace lux::render;
using lux::rdesc::ETextureFormat;
static int failures = 0;

static void check(bool condition, const char* label)
{
    if (!condition)
    {
        std::cerr << label << '\n';
        ++failures;
    }
}

static RenderResult<RenderGraphDefinition> ranged(BufferRange uniform, BufferRange storage, ImageRange image)
{
    RenderGraphBuilder builder;
    Stages value;
    value.vertex.buffer = builder.importBuffer("fixture.vertex", {256, 16}, EPersistentScope::SCENE);
    value.shared.buffer = builder.importBuffer("fixture.shared", {256, 16}, EPersistentScope::SCENE);
    value.fragment.buffer = builder.importBuffer("fixture.fragment", {256, 16}, EPersistentScope::SCENE);
    value.vertex.range = uniform;
    value.fragment.range = storage;
    TextureDesc desc;
    desc.mip_count = 5;
    desc.array_layers = 4;
    value.image.texture = builder.importTexture("fixture.image", desc, EPersistentScope::VIEW);
    value.image.range = image;
    value.linear.sampler = GraphSampler{1};
    const auto pass = builder.addPass(
        "fixture.graphics",
        {"shaders/fixture", "default"},
        EPassKind::GRAPHICS,
        EExecutionScope::VIEW,
        value
    );
    if (!pass)
    {
        return lux::cxx::unexpected(pass.error());
    }
    return std::move(builder).finish();
}

static void ranges()
{
    static_assert(sizeof(StageUniform) == 64);
    const auto good = ranged(UniformBuffer<StageUniform>{}.range, {}, SampledTexture{}.range);
    check(bool(good), "defaults produce bounded complete Shader access");
    if (good)
    {
        const auto& pass = good->passes()[0];
        check(std::get<BufferRange>(pass.uses[0].range).byte_count == 64, "64-byte UBO default is 64 bytes");
        check(
            std::get<BufferRange>(pass.uses[2].range).byte_count == 256 && pass.uses[2].element_stride == 16,
            "runtime SSBO resolves to 16 bounded elements"
        );
        check(
            std::get<ImageRange>(pass.uses[3].range).mip_count == 5 &&
                std::get<ImageRange>(pass.uses[3].range).layer_count == 4,
            "implicit LOD default covers every mip and layer"
        );
        for (const auto& use : pass.uses)
        {
            const auto& field = pass.bindings[use.field_index];
            check(
                (std::holds_alternative<ImageRange>(use.range)
                     ? field.image_range == std::get<ImageRange>(use.range)
                     : field.buffer_range == std::get<BufferRange>(use.range)),
                "binding and dependency use identical finite range"
            );
        }
    }
    const auto narrow = ranged({64, 64}, {32, 64}, {EAspect::COLOR, 1, 2, 1, 2});
    check(bool(narrow), "explicit UBO/SSBO/native view ranges accepted");
    if (narrow)
    {
        check(
            std::get<BufferRange>(narrow->passes()[0].uses[2].range) == BufferRange{32, 64},
            "SSBO byte range preserved"
        );
        check(
            narrow->passes()[0].bindings[3].image_range == ImageRange{EAspect::COLOR, 1, 2, 1, 2},
            "explicit native view does not widen or narrow silently"
        );
    }
    for (const BufferRange bad : {BufferRange{0, 1}, BufferRange{4, 64}, BufferRange{240, 64}, BufferRange{0, 0}})
    {
        const auto result = ranged(bad, {}, SampledTexture{}.range);
        check(!result && result.error().type == kGraphInvalidUse, "UBO undersize/alignment/bounds/empty rejected");
    }
    for (const BufferRange bad : {BufferRange{0, 17}, BufferRange{4, 64}, BufferRange{257, kRemainingBytes}})
    {
        check(!ranged({0, 64}, bad, SampledTexture{}.range), "SSBO stride/alignment/bounds rejected");
    }
    check(!ranged({0, 64}, {}, {EAspect::COLOR, 4, 2, 0, 1}), "mip view bounds rejected");
    check(!ranged({0, 64}, {}, {EAspect::COLOR, 0, 1, 4, kRemainingSubresources}), "empty remaining layers rejected");
}

static void formats()
{
    using namespace lux::rdesc;
    check(textureFormatClass(ETextureFormat::RGBA16_SFLOAT) == ETextureFormatClass::COLOR, "HDR classification");
    check(supportsTextureUsage(ETextureFormat::RGBA8_SRGB, ETextureUsage::COLOR_ATTACHMENT), "sRGB color target");
    check(!supportsTextureUsage(ETextureFormat::RGBA8_SRGB, ETextureUsage::STORAGE), "sRGB storage rejected");
    check(!supportsTextureUsage(ETextureFormat::D32_SFLOAT, ETextureUsage::COLOR_ATTACHMENT), "depth color rejected");
    check(
        !supportsTextureUsage(ETextureFormat::BC7_UNORM, ETextureUsage::COLOR_ATTACHMENT),
        "compressed target rejected"
    );
    for (const auto format :
         {ETextureFormat::RGBA16_SFLOAT,
          ETextureFormat::RGBA8_SRGB,
          ETextureFormat::D16_UNORM,
          ETextureFormat::D16_UNORM_S8_UINT,
          ETextureFormat::D24_UNORM_S8_UINT,
          ETextureFormat::D32_SFLOAT_S8_UINT})
    {
        RenderGraphBuilder builder;
        TextureDesc desc;
        desc.format = format;
        (void)builder.texture(desc, "fixture.format");
        check(bool(std::move(builder).finish()), "actual Legacy format declared without ordinal assumptions");
    }
    for (const auto format : {ETextureFormat::RGBA16_SFLOAT, ETextureFormat::RGBA8_SRGB, ETextureFormat::D32_SFLOAT})
    {
        RenderGraphBuilder builder;
        HalfStorage params;
        params.values.buffer = builder.importBuffer("storage", {64, 4}, EPersistentScope::SCENE);
        TextureDesc desc;
        desc.format = format;
        params.output.texture = builder.texture(desc);
        check(
            bool(builder.addPass("half.storage", {"storage", "half"}, EPassKind::COMPUTE, EExecutionScope::VIEW, params)
            ),
            "capture storage format candidate"
        );
        check(
            bool(std::move(builder).finish()) == (format == ETextureFormat::RGBA16_SFLOAT),
            "half-float storage accepted, sRGB/depth storage rejected"
        );
    }
    for (const auto format :
         {ETextureFormat::D16_UNORM_S8_UINT,
          ETextureFormat::D24_UNORM_S8_UINT,
          ETextureFormat::D32_SFLOAT_S8_UINT,
          ETextureFormat::RGBA8_UNORM})
    {
        RenderGraphBuilder builder;
        Attachments params;
        TextureDesc color;
        color.samples = 4;
        params.color.texture = builder.texture(color);
        params.resolved.texture = builder.texture({});
        color.format = format;
        params.depth.texture = builder.texture(color);
        params.depth.range.aspect = EAspect::DEPTH_STENCIL;
        check(
            bool(builder.addPass("depth", {"depth", "default"}, EPassKind::GRAPHICS, EExecutionScope::VIEW, params)),
            "capture depth-stencil candidate"
        );
        check(
            bool(std::move(builder).finish()) == (format != ETextureFormat::RGBA8_UNORM),
            "all authoritative depth-stencil formats have legal combined attachment aspects"
        );
    }
    for (const auto format : {ETextureFormat::UNDEFINED, static_cast<ETextureFormat>(9999)})
    {
        RenderGraphBuilder builder;
        TextureDesc desc;
        desc.format = format;
        (void)builder.texture(desc);
        check(!std::move(builder).finish(), "undefined/unknown format rejected");
    }
    for (const auto format : {ETextureFormat::RGBA16_SFLOAT, ETextureFormat::RGBA8_SRGB, ETextureFormat::D32_SFLOAT})
    {
        RenderGraphBuilder builder;
        Tonemap value;
        value.input.texture = builder.importTexture("color.input", {}, EPersistentScope::VIEW);
        value.linear.sampler = GraphSampler{1};
        TextureDesc desc;
        desc.format = format;
        value.output.texture = builder.texture(desc);
        check(
            bool(
                builder.addPass("color.pass", {"tonemap", "default"}, EPassKind::GRAPHICS, EExecutionScope::VIEW, value)
            ),
            "capture color use"
        );
        check(bool(std::move(builder).finish()) == (format != ETextureFormat::D32_SFLOAT), "color usage validation");
    }
}

static void identities()
{
    static_assert(passKey("feature.pass") == passKey("feature.pass"));
    static_assert(passKey("feature.pass") != passKey("other.pass"));
    static_assert(graphResourceKey("scene.color").isValid());
    check(shaderKey({"shader/tonemap", "hdr"}) != shaderKey({"shader/tonemap", "ldr"}), "variant is identity");
    check(!shaderKey({"shader#ambiguous", "hdr"}).isValid(), "ambiguous canonical separator rejected");
    RenderGraphBuilder builder;
    {
        std::string name = "feature.scene.color";
        (void)builder.importTexture(name, {}, EPersistentScope::SCENE);
    }
    const auto good = std::move(builder).finish();
    check(
        good && good->resources()[0].canonical_name == "feature.scene.color" &&
            good->resources()[0].semantic == graphResourceKey("feature.scene.color"),
        "import identity owns name and scope"
    );
    RenderGraphBuilder duplicate;
    (void)duplicate.texture({}, "same.semantic");
    (void)duplicate.buffer({}, "same.semantic");
    check(!std::move(duplicate).finish(), "cross Feature duplicate semantic requires composition resolution");
    RenderGraphBuilder unnamed;
    (void)unnamed.importTexture({}, {}, EPersistentScope::VIEW);
    check(!std::move(unnamed).finish(), "imports require semantic name");
    RenderGraphBuilder unscoped;
    (void)unscoped.importBuffer("unscoped", {}, EPersistentScope::NONE);
    check(!std::move(unscoped).finish(), "imports require persistent scope");
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        return 2;
    }
    const std::string_view mode = argv[1];
    if (mode == "ranges")
    {
        ranges();
    }
    else if (mode == "formats")
    {
        formats();
    }
    else if (mode == "identities")
    {
        identities();
    }
    else
    {
        return 2;
    }
    std::cout << mode << " failures=" << failures << '\n';
    return failures != 0;
}
