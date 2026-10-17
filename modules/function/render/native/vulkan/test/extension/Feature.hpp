#pragma once

#include <lux/engine/render/features/Registration.hpp>

#include "Buffer.hpp"
#include "Shader.hpp"
#include "../shaders/Texture.hpp"
#include <lux/engine/render/features/Assembly.hpp>
#include <lux/engine/render/resource/Texture.hpp>

namespace extension_test
{
    struct TestBufferFeature final {};
    struct ViewConfig final { TextureId texture; BufferId buffer; };
}

namespace extension_test
{
    inline constexpr FeatureName feature_type("test.extension.BufferFeature");
    inline constexpr EncodedDataLimits limits{16, 0};
    inline const TargetDesc target{16, 16, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};

    inline RenderResult<void> describe(const void*, std::string_view, std::vector<ResourceClaim>& out) noexcept
    {
        out.push_back({"texture", EResourceClaim::PROVIDE, GraphImageDesc{16, 16}, EGraphOrigin::IMPORTED,
            {GraphImageRange{1, 0, 1, 0, 1}},
            StorageRequirement{StorageName(kTextureStorageType.name()), EResourceScope::GLOBAL, "default"}});
        out.push_back({"values", EResourceClaim::PROVIDE, GraphBufferDesc{16}, EGraphOrigin::IMPORTED,
            {GraphBufferRange{0, 16}},
            StorageRequirement{StorageName(TestBufferStorage::description.type.name()),
                EResourceScope::GLOBAL, "values"}});
        out.push_back({"color", EResourceClaim::PROVIDE, GraphImageDesc{16, 16}});
        return {};
    }

    inline RenderResult<void> build(const void*, std::string_view name,
        std::span<const ResolvedResource> resources, RenderGraphBuilder& graph) noexcept
    {
        const auto texture = findResource(resources, "texture")->version;
        const auto buffer = findResource(resources, "values")->version;
        const auto color = findResource(resources, "color")->version;
        auto pixels = graph.declareResource({featureKey(feature_type, name, "pixels"), GraphBufferDesc{1024}});
        if (!pixels) return lux::cxx::unexpected(pixels.error());
        auto draw = graph.addPass({featureKey(feature_type, name, "draw"), EGraphPassKind::GRAPHICS,
            {{texture, {}, EGraphAccess::READ, 0, {}, EGraphUse::SAMPLED},
             {buffer, {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
             {color, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}});
        if (!draw) return lux::cxx::unexpected(draw.error());
        auto read = graph.addPass({featureKey(feature_type, name, "readback"), EGraphPassKind::TRANSFER,
            {{color, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{*pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}});
        if (!read) return lux::cxx::unexpected(read.error());
        if (auto exported = graph.exportOutput({color, {}, target}); !exported) return exported;
        return graph.exportOutput({{*pixels, 1}});
    }

    inline const FeatureDescriptor definition{feature_type, ConfigValue::typeOf<ViewConfig>()};
    inline const FeatureGraphOps definition_graph{describe, build};

    inline RenderResult<ProgramContribution> programs(const void* wire, std::string_view name) noexcept
    {
        const auto* config = static_cast<const ViewConfig*>(wire);
        ProgramContribution result;
        PassProgram draw{featureKey(feature_type, name, "draw")};
        draw.shader = {{texture_fixture::vert.begin(), texture_fixture::vert.end()}, {fragment.begin(), fragment.end()},
            {{{0, 0, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0}, {1, 0, EDescriptorKind::STORAGE_BUFFER, 1, 16}}, 16}};
        result.arguments.push_back({"shared:texture", *encodeData(TextureBinding{config->texture}, {}, {12, 0})});
        result.arguments.push_back({"shared:values", *encodeData(BufferBinding{config->buffer}, {}, {8, 0})});
        result.programs.push_back(std::move(draw));
        result.programs.push_back({featureKey(feature_type, name, "readback")});
        return result;
    }
}
