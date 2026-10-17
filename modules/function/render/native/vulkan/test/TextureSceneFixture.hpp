#pragma once

#include <lux/engine/render/data/Encoding.hpp>

#include "shaders/Texture.hpp"
#include <lux/engine/render/features/Assembly.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/resource/Texture.hpp>

namespace texture_fixture
{
    using namespace lux::render;
    namespace cxx = lux::cxx;
    inline constexpr FeatureName type("test.sampled_texture");
    inline constexpr EncodedDataLimits limits{4096, 4096};
    struct Config final
    {
        TextureId texture;
        std::uint32_t width = 16, height = 16, mips = 1;
        lux::rdesc::ETextureFormat format = lux::rdesc::ETextureFormat::RGBA8_UNORM;
        std::uint32_t lod = 0;
    };
}

namespace texture_fixture
{
    inline TargetDesc target(Config c) noexcept
    {
        return {std::max(1u, c.width >> c.lod), std::max(1u, c.height >> c.lod),
            lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    }

    inline RenderResult<void> describe(const void* wire, std::string_view, std::vector<ResourceClaim>& out) noexcept
    {
        const auto* c = static_cast<const Config*>(wire);
        const bool is_invalid = !c->width || !c->height || !c->mips || c->mips > 16 || c->lod >= c->mips;
        if (is_invalid) return cxx::unexpected(RenderError{kFeatureContract});
        out.push_back({"texture", EResourceClaim::PROVIDE,
            GraphImageDesc{c->width, c->height, c->mips, 1, c->format}, EGraphOrigin::IMPORTED,
            {GraphImageRange{1, 0, c->mips, 0, 1}},
            StorageRequirement{StorageName(kTextureStorageType.name()), {1, 0}, EResourceScope::GLOBAL, "default"}});
        const auto t = target(*c);
        out.push_back({"color", EResourceClaim::PROVIDE, GraphImageDesc{t.width, t.height}});
        return {};
    }

    inline RenderResult<void> build(const void* wire, std::string_view instance,
        std::span<const ResolvedResource> resources, RenderGraphBuilder& builder) noexcept
    {
        const auto* c = static_cast<const Config*>(wire);
        const auto t = target(*c);
        const auto color = findResource(resources, "color")->version;
        const auto texture = findResource(resources, "texture")->version;
        auto pixels = builder.declareResource({featureKey(type, instance, "pixels"),
            GraphBufferDesc{std::uint64_t(t.width) * t.height * 4}});
        if (!pixels) return cxx::unexpected(pixels.error());
        auto draw = builder.addPass({featureKey(type, instance, "draw"), EGraphPassKind::GRAPHICS,
            {{texture, {}, EGraphAccess::READ, 0, {}, EGraphUse::SAMPLED},
             {color, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}});
        if (!draw) return cxx::unexpected(draw.error());
        auto read = builder.addPass({featureKey(type, instance, "readback"), EGraphPassKind::TRANSFER,
            {{color, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{*pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}});
        if (!read) return cxx::unexpected(read.error());
        if (auto r = builder.exportOutput({color, {}, t}); !r) return r;
        return builder.exportOutput({{*pixels, 1}});
    }

    inline const FeatureDescriptor definition{type, ConfigValue::typeOf<Config>()};
    inline const FeatureGraphOps definition_graph{describe, build};

    inline std::vector<PassProgram> programs(Config c, std::string_view instance = "main")
    {
        PassProgram draw{featureKey(type, instance, "draw")};
        draw.shader = {{vert.begin(), vert.end()}, {frag.begin(), frag.end()}, {{{0, 0, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0}}, 16}};
        std::vector<PassProgram> result;
        result.push_back(std::move(draw));
        result.push_back({featureKey(type, instance, "readback")});
        return result;
    }

    inline std::vector<StorageArgument> arguments(Config c)
    {
        return {{"shared:texture", *encodeData(TextureBinding{c.texture, c.lod}, {}, {12, 0})}};
    }

    inline RenderResult<FeatureAssembly> assemble(Config c)
    {
        auto value = ConfigValue::make(c);
        if (!value) return cxx::unexpected(value.error());
        const FeatureInstanceView selected{&definition, &definition_graph, &*value, "main"};
        return assembleFeatures({&selected, 1}, limits.max_encoded_bytes);
    }
}
