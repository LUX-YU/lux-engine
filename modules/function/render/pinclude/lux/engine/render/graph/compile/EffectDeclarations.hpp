#pragma once

#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectResourceManifest.hpp>
#include <lux/engine/render/graph/compile/GraphAnalysis.hpp>
#include <lux/engine/render/graph/TargetImageSlot.hpp>
#include <algorithm>
#include <vector>

namespace lux::render::effect_detail { struct ValidatedEffectNode; }

namespace lux::render::runtime_detail
{
    struct DeclaredResourceRequirement final
    {
        std::string binding;
        ResourceName type;
        EResourceDomain domain;
        EResourceContentLifetime lifetime;
        bool scope_default;
    };

    struct EffectResourceOperand final
    {
        struct ViewContract final
        {
            cxx::basic_type_info::id_t hash;
            std::string name;
            bool operator==(const ViewContract&) const noexcept = default;
        };

        std::string binding;
        ResourceName type;
        std::variant<ResourceProjection, ViewContract, DrawResourceReads> contract;
        std::uint32_t projection;
        bool optional = false;
        std::uint32_t graph_uses = 0;
        detail::ImportedResourceSlot slot{};
        std::uint32_t requirement = kGraphInvalidIndex;
    };

    struct ConstantBufferDeclaration final
    {
        const effect_detail::ValidatedEffectNode* node;
        GraphResourceId resource;
        extension_detail::ConstantBufferField source;
    };

    struct EffectDeclarations final
    {
        struct Buffer final
        {
            GraphResourceId resource;
            graph::NodeId node;
            std::uint64_t minimum_bytes, maximum_bytes;
            bool upload;
            std::uint32_t size_requirement = kGraphInvalidIndex;
            std::uint32_t size_field = 0;
        };

        struct Algorithm final
        {
            std::uint32_t source_index;
            FrameAlgorithmUses declaration;
            graph::NodeId node;
            ConfigValue configuration;
            extension_detail::BindFrameAlgorithm bind;
        };

        struct RasterSource final
        {
            GraphPassId pass;
            std::uint32_t programs = kGraphInvalidIndex, draw_list = kGraphInvalidIndex;
        };

        struct InlineCopy final
        {
            const effect_detail::ValidatedEffectNode* node;
            GraphPassId pass;
            extension_detail::InlineBlockInput input;
        };

        std::vector<detail::TargetImageSlot> target_imports;
        std::vector<DeclaredResourceRequirement> requirements;
        std::vector<EffectResourceOperand> operands;
        std::vector<RasterSource> raster_sources;
        EffectResourceManifest resources;
        std::vector<InlineCopy> inline_copies;
        std::vector<Buffer> buffers;
        std::vector<Algorithm> algorithms;
        std::vector<ConstantBufferDeclaration> uniforms;

        // Buffer declarations follow increasing draft resource IDs; a rejected branch truncates both tables.
        [[nodiscard]] const Buffer* findBuffer(GraphResourceId id) const noexcept
        {
            const auto index = [](const Buffer& buffer) noexcept { return buffer.resource.index; };
            const auto row = std::ranges::lower_bound(buffers, id.index, {}, index);
            return row != buffers.end() && row->resource == id ? &*row : nullptr;
        }
    };

    [[nodiscard]] bool sameBlockContract(ConstantBlockContract, ConstantBlockContract, rdesc::EShaderTarget) noexcept;
    [[nodiscard]] RenderResult<RenderGraphAnalysis>
    analyzeFrameAlgorithms(const GraphDefinition&, EffectDeclarations&, rdesc::EShaderTarget) noexcept;

    [[nodiscard]] RenderResult<RenderGraphAnalysis>
    analyzeEffectResources(const GraphDefinition&, EffectDeclarations&, rdesc::EShaderTarget) noexcept;
}
