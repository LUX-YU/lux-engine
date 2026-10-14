#pragma once

#include "shaders/Triangle.hpp"
#include <lux/engine/render/features/Feature.hpp>
#include <lux/engine/render/native/vulkan/Program.hpp>
#include <atomic>

namespace native_fixture
{
    using namespace lux::render;
    namespace cxx = lux::cxx;
    inline std::atomic<unsigned> descriptions{0}, builds{0};
    inline constexpr FeatureTypeIdView type("test.offscreen");
    inline constexpr WireLimits limits{4096, 4096};

    struct Config final { std::uint32_t width = 32, height = 32, graphics = 1, scopes = 0; };
}

namespace lux::render
{
    template <> struct DataSchema<native_fixture::Config>
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{
                {"width", cxx::EScalarKind::U32}, {"height", cxx::EScalarKind::U32},
                {"graphics", cxx::EScalarKind::U32}, {"scopes", cxx::EScalarKind::U32}
            };
            static const DataSchemaDescriptor schema{
                DataTypeIdView("test.offscreen.config"), {1, 0}, {1, 0}, fields, 16, 4
            };
            return schema;
        }
        static RenderResult<std::size_t> wireSize(const native_fixture::Config&) noexcept { return 16; }

        static RenderResult<void> encode(const native_fixture::Config& value, cxx::BinarySpanWriter& out) noexcept
        {
            for (auto x : {value.width, value.height, value.graphics, value.scopes})
                out.writeUnsigned(x);
            return {};
        }
        static RenderResult<native_fixture::Config> decode(cxx::BinaryReader& in, WireView) noexcept
        {
            native_fixture::Config value;
            for (auto* x : {&value.width, &value.height, &value.graphics, &value.scopes})
            {
                in.readUnsigned(*x);
            }
            if (value.graphics > 1 || value.scopes > 1) return cxx::unexpected(RenderError{kInvalidWire});
            return value;
        }
    };
}

namespace native_fixture
{
    inline TargetDesc target(Config c) noexcept
    {
        return {c.width, c.height, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    }

    inline RenderResult<void> describe(const WireView* wire, std::string_view, std::vector<ResourceClaim>& claims) noexcept
    {
        ++descriptions;
        auto c = decodeWire<Config>(*wire, limits);
        if (!c) return cxx::unexpected(c.error());
        claims.push_back({"color", EResourceClaim::PROVIDE, GraphImageDesc{c->width, c->height}});
        if (c->scopes)
            for (unsigned i = 0; i < 4; ++i)
                claims.push_back({"scope" + std::to_string(i), EResourceClaim::PROVIDE, GraphBufferDesc{16},
                    EGraphOrigin::IMPORTED, {GraphBufferRange{0, 16}},
                    StorageRequirement{StorageTypeId("test.zero_buffer"), {1, 0}, static_cast<EResourceScope>(i)}});
        return {};
    }

    inline RenderResult<void> build(const WireView* wire, std::string_view instance,
        std::span<const ResolvedResource> resources, RenderGraphBuilder& builder) noexcept
    {
        ++builds;
        auto c = decodeWire<Config>(*wire, limits);
        if (!c) return cxx::unexpected(c.error());
        const auto color = findResource(resources, "color")->version;
        auto pixels = builder.declareResource({featureKey(type, instance, "pixels"),
            GraphBufferDesc{std::uint64_t(c->width) * c->height * 4}});
        if (!pixels) return cxx::unexpected(pixels.error());
        const auto kind = c->graphics ? EGraphPassKind::GRAPHICS : EGraphPassKind::TRANSFER;
        const auto usage = c->graphics ? EGraphUse::COLOR_ATTACHMENT : EGraphUse::TRANSFER_DESTINATION;
        if (auto p = builder.addPass({featureKey(type, instance, "color"), kind,
            {{color, {}, EGraphAccess::WRITE, 0, {}, usage}}}); !p) return cxx::unexpected(p.error());
        if (auto p = builder.addPass({featureKey(type, instance, "readback"), EGraphPassKind::TRANSFER,
            {{color, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{*pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}); !p)
            return cxx::unexpected(p.error());
        if (auto r = builder.exportOutput({color, {}, target(*c)}); !r) return r;
        if (auto r = builder.exportOutput({{*pixels, 1}}); !r) return r;
        if (c->scopes)
            for (unsigned i = 0; i < 4; ++i)
            {
                const auto key = "scope" + std::to_string(i);
                const auto source = findResource(resources, key)->version;
                auto copy = builder.declareResource({featureKey(type, instance, key + ".copy"), GraphBufferDesc{16}});
                if (!copy) return cxx::unexpected(copy.error());
                auto p = builder.addPass({featureKey(type, instance, key), EGraphPassKind::TRANSFER,
                    {{source, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
                     {{*copy, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}});
                if (!p) return cxx::unexpected(p.error());
                if (auto r = builder.exportOutput({{*copy, 1}}); !r) return r;
            }
        return {};
    }

    inline const FeatureDefinition definition{type, {1, 0}, &DataSchema<Config>::describe(), describe, build};

    inline std::vector<vulkan::PassProgram> programs(Config config, std::string_view instance = "main")
    {
        std::vector<vulkan::PassProgram> result;
        vulkan::PassProgram color{featureKey(type, instance, "color")};
        if (config.graphics) color.shader = {{vert.begin(), vert.end()}, {frag.begin(), frag.end()}, 1};
        color.clear_color = {0.125f, 0.25f, 0.5f, 1};
        result.push_back(std::move(color));
        result.push_back({featureKey(type, instance, "readback")});
        if (config.scopes)
            for (unsigned i = 0; i < 4; ++i)
                result.push_back({featureKey(type, instance, "scope" + std::to_string(i))});
        return result;
    }

    inline RenderResult<FeatureAssembly> assemble(Config config)
    {
        auto bytes = encodeWire(config, {}, limits);
        if (!bytes) return cxx::unexpected(bytes.error());
        const WireView wire{DataSchema<Config>::describe(), *bytes};
        const SelectedFeature selected{&definition, &wire, "main"};
        return assembleFeatures({&selected, 1}, limits);
    }
}
