#pragma once

#include "shaders/Texture.hpp"
#include <lux/engine/render/features/Feature.hpp>
#include <lux/engine/render/features/Program.hpp>
#include <lux/engine/render/features/Texture.hpp>

namespace texture_fixture
{
    using namespace lux::render;
    namespace cxx = lux::cxx;
    inline constexpr FeatureTypeIdView type("test.sampled_texture");
    inline constexpr WireLimits limits{4096, 4096};
    struct Config final
    {
        TextureId texture;
        std::uint32_t width = 16, height = 16, mips = 1;
        lux::rdesc::ETextureFormat format = lux::rdesc::ETextureFormat::RGBA8_UNORM;
        std::uint32_t lod = 0;
    };
}

namespace lux::render
{
    template <> struct DataSchema<texture_fixture::Config>
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{
                {"index", cxx::EScalarKind::U32}, {"generation", cxx::EScalarKind::U32},
                {"width", cxx::EScalarKind::U32}, {"height", cxx::EScalarKind::U32},
                {"mips", cxx::EScalarKind::U32}, {"format", cxx::EScalarKind::U32},
                {"lod", cxx::EScalarKind::U32}
            };
            static const DataSchemaDescriptor schema{
                DataTypeIdView("test.sampled_texture.config"), {1, 0}, {1, 0}, fields, 28, 4
            };
            return schema;
        }

        static RenderResult<std::size_t> wireSize(const texture_fixture::Config&) noexcept { return 28; }

        static RenderResult<void> encode(const texture_fixture::Config& value, cxx::BinarySpanWriter& out) noexcept
        {
            for (auto x : {value.texture.index, value.texture.gen, value.width, value.height, value.mips,
                static_cast<std::uint32_t>(value.format), value.lod}) out.writeUnsigned(x);
            return {};
        }

        static RenderResult<texture_fixture::Config> decode(cxx::BinaryReader& in, WireView) noexcept
        {
            texture_fixture::Config value;
            std::uint32_t format = 0;
            for (auto* x : {&value.texture.index, &value.texture.gen, &value.width, &value.height,
                &value.mips, &format, &value.lod}) in.readUnsigned(*x);
            value.format = static_cast<rdesc::ETextureFormat>(format);
            const bool is_invalid = !value.width || !value.height || !value.mips || value.mips > 16 ||
                value.lod >= value.mips;
            if (is_invalid) return cxx::unexpected(RenderError{kInvalidWire});
            return value;
        }
    };
}

namespace texture_fixture
{
    inline TargetDesc target(Config c) noexcept
    {
        return {std::max(1u, c.width >> c.lod), std::max(1u, c.height >> c.lod),
            lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    }

    inline RenderResult<void> describe(const WireView* wire, std::string_view, std::vector<ResourceClaim>& out) noexcept
    {
        auto c = decodeWire<Config>(*wire, limits);
        if (!c) return cxx::unexpected(c.error());
        out.push_back({"texture", EResourceClaim::PROVIDE,
            GraphImageDesc{c->width, c->height, c->mips, 1, c->format}, EGraphOrigin::IMPORTED,
            {GraphImageRange{1, 0, c->mips, 0, 1}},
            StorageRequirement{StorageTypeId(kTextureStorageType.name()), {1, 0}, EResourceScope::GLOBAL, "default"}});
        const auto t = target(*c);
        out.push_back({"color", EResourceClaim::PROVIDE, GraphImageDesc{t.width, t.height}});
        return {};
    }

    inline RenderResult<void> build(const WireView* wire, std::string_view instance,
        std::span<const ResolvedResource> resources, RenderGraphBuilder& builder) noexcept
    {
        auto c = decodeWire<Config>(*wire, limits);
        if (!c) return cxx::unexpected(c.error());
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

    inline const FeatureDefinition definition{type, {1, 0}, &DataSchema<Config>::describe(), describe, build};

    inline std::vector<PassProgram> programs(Config c, std::string_view instance = "main")
    {
        PassProgram draw{featureKey(type, instance, "draw")};
        draw.shader = {{vert.begin(), vert.end()}, {frag.begin(), frag.end()}, {{{0, 0, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0}}, 16}};
        draw.arguments.push_back({"shared:texture", *encodeWire(TextureBinding{c.texture, c.lod}, {}, {12, 0})});
        std::vector<PassProgram> result;
        result.push_back(std::move(draw));
        result.push_back({featureKey(type, instance, "readback")});
        return result;
    }

    inline std::vector<StorageArgument> arguments(Config c) { return *programArguments(programs(c)); }

    inline RenderResult<FeatureAssembly> assemble(Config c)
    {
        auto bytes = encodeWire(c, {}, limits);
        if (!bytes) return cxx::unexpected(bytes.error());
        const WireView wire{DataSchema<Config>::describe(), *bytes};
        const SelectedFeature selected{&definition, &wire, "main"};
        return assembleFeatures({&selected, 1}, limits);
    }
}
