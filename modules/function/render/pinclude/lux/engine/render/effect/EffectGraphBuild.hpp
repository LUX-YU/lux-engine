#pragma once

#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/effect/EffectCatalog.hpp>
#include <lux/engine/render/effect/EffectGraph.hpp>
#include <lux/engine/render/effect/EffectPreparation.hpp>
#include <lux/engine/render/core/Resource.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/effect/EffectUniformBlock.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/graph/TargetImageSlot.hpp>
#include <span>
#include <string>

namespace lux::render::runtime_detail
{
    class CommandRegistry;
    struct RenderEffect;

    struct EffectNodeConfiguration final
    {
        graph::NodeId node;
        ConfigValue value;
    };

    struct EffectStorageImport final
    {
        struct SnapshotContract final
        {
            cxx::basic_type_info::id_t hash;
            std::string name;
            bool operator==(const SnapshotContract&) const noexcept = default;
        };

        enum class ERasterOwner : std::uint8_t
        {
            TEXTURES,
            MESHES,
            MATERIALS
        };

        std::string binding;
        ResourceName type;
        std::variant<detail::StorageGraphImport, SnapshotContract, ERasterOwner> contract;
        std::uint32_t canonical;
        std::uint32_t projection;
        bool operator==(const EffectStorageImport&) const noexcept = default;
    };

    struct EffectImportContracts final
    {
        std::vector<EffectStorageImport> storages;
        bool operator==(const EffectImportContracts&) const noexcept = default;
    };

    struct UniformBlockDeclaration final
    {
        graph::NodeId node;
        std::string name;
        GraphResourceId resource;
        extension_detail::EffectUniformSource source;
    };

    struct EffectGraphBuild final
    {
        RenderGraph source;
        RenderGraphAnalysis analysis;
        EffectOutputDesc output;
        std::vector<detail::TargetImageSlot> target_imports;
        std::vector<GraphResourceId> storage_imports;
        PreparationDefinition preparation;
        std::vector<UniformBlockDeclaration> uniforms;
    };

    [[nodiscard]] RenderResult<void> initializeEffectGraph(
        RenderEffect&, std::unique_ptr<EffectGraph>, const EffectCatalog&, const CommandRegistry&,
        std::size_t max_config_bytes, const RenderErrorSink&
    ) noexcept;

    // The immutable author inputs borrowed for one lowering call on the compile worker.
    struct EffectRecipeView final
    {
        const EffectGraph& graph;
        std::span<const EffectNodeConfiguration> configurations;
    };

    struct EffectBuildContract final
    {
        const EffectOutputDesc& target;
        const EffectBuildDevice& device;
        const EffectImportContracts& imports;
    };

    [[nodiscard]] RenderResult<EffectGraphBuild>
    lowerEffectGraph(EffectRecipeView, const EffectCatalog&, EffectBuildContract, const RenderErrorSink&) noexcept;
}
