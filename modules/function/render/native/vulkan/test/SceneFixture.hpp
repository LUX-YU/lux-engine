#pragma once

#include <lux/engine/render/data/Encoding.hpp>

#include "shaders/Triangle.hpp"
#include <lux/engine/render/features/Assembly.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <atomic>

namespace native_fixture
{
    using namespace lux::render;
    namespace cxx = lux::cxx;
    inline std::atomic<unsigned> descriptions{0}, builds{0};
    inline constexpr FeatureName type("test.offscreen");
    inline constexpr EncodedDataLimits limits{4096, 4096};

    struct Config final { std::uint32_t width = 32, height = 32, graphics = 1, scopes = 0; };
}

namespace native_fixture
{
    inline TargetDesc target(Config c) noexcept
    {
        return {c.width, c.height, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    }

    inline RenderResult<void> describe(const void* wire, std::string_view, ResourceClaims& claims) noexcept
    {
        ++descriptions;
        const auto* c = static_cast<const Config*>(wire);
        if (c->graphics > 1 || c->scopes > 1) return cxx::unexpected(RenderError{kFeatureContract});
        claims.resources.push_back({"color", EResourceClaim::PROVIDE, GraphImageDesc{c->width, c->height}});
        if (c->scopes)
        {
            const std::array groups{&claims.storage.global, &claims.storage.scene,
                &claims.storage.view, &claims.storage.attachment};
            for (unsigned i = 0; i < groups.size(); ++i)
                groups[i]->push_back({"scope" + std::to_string(i), EResourceClaim::PROVIDE, GraphBufferDesc{16},
                    EGraphOrigin::IMPORTED, {GraphBufferRange{0, 16}},
                    StorageRequirement{StorageName("test.zero_buffer")}});
        }
        return {};
    }

    inline RenderResult<void> build(const void* wire, std::string_view instance,
        std::span<const ResolvedResource> resources, RenderGraphBuilder& builder) noexcept
    {
        ++builds;
        const auto* c = static_cast<const Config*>(wire);
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

    inline const FeatureDescriptor definition{type, ConfigValue::typeOf<Config>()};
    inline const FeatureGraphOps definition_graph{describe, build};

    inline std::vector<PassProgram> programs(Config config, std::string_view instance = "main")
    {
        std::vector<PassProgram> result;
        PassProgram color{featureKey(type, instance, "color")};
        if (config.graphics) color.shader = {{vert.begin(), vert.end()}, {frag.begin(), frag.end()}, {}};
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
        auto value = ConfigValue::make(config);
        if (!value) return cxx::unexpected(value.error());
        const FeatureInstanceView selected{&definition, &definition_graph, &*value, "main"};
        return assembleFeatures({&selected, 1}, limits.max_encoded_bytes);
    }
}
