#pragma once

#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/effect/EffectUniformBlock.hpp>
#include <lux/engine/render/extensions/execution/GraphPreparation.hpp>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <lux/engine/render/extensions/graph/Work.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace lux::render
{
    class RenderGraphBuilder;

    struct EffectResourceValue final
    {
        graph::PinSemanticId member;
        ResourceVersion version;
    };

    using EffectResourceBundle = std::vector<EffectResourceValue>;

    struct EffectBuildDevice final
    {
        rdesc::EShaderTarget shader_target;
        bool buffer_device_address;
        bool indirect_first_instance;
        std::array<std::uint32_t, 3> compute_dispatch_limit;
        std::uint64_t storage_buffer_max_range;
        std::uint32_t texture_layer_limit;
    };

    // Short-lived contribution context for one device/target variant. Bundles hold graph versions
    // which remain valid only for this build. The context never exposes mutable logical state.
    class EffectBuildContext
    {
    public:
        virtual ~EffectBuildContext() noexcept = default;
        [[nodiscard]] virtual graph::NodeId nodeId() const noexcept = 0;
        // Borrows the immutable value published during CreateEffect for this build call.
        template <class T> [[nodiscard]] const T* configuration() const & noexcept
        {
            const auto* configuration = nodeConfiguration();
            return configuration ? configuration->borrow<T>() : nullptr;
        }

        template <class T> const T* configuration() const && = delete;

        // Extents are evaluated per instance from graph declarations, including through compatible resize.
        [[nodiscard]] virtual const EffectOutputDesc& target() const noexcept = 0;
        [[nodiscard]] virtual const EffectBuildDevice& device() const noexcept = 0;
        [[nodiscard]] rdesc::EShaderTarget shaderTarget() const noexcept { return device().shader_target; }
        [[nodiscard]] virtual RenderGraphBuilder& builder() noexcept = 0;
        // Read-only sampling of an explicitly bound, same-device offscreen Target.
        [[nodiscard]] virtual RenderResult<ResourceVersion> importTargetImage(
            std::string_view binding, rdesc::ETextureFormat format, std::uint32_t samples = 1
        ) noexcept = 0;
        [[nodiscard]] virtual RenderResult<ResourceVersion>
        importResource(
            std::string_view binding, const ResourceRequirement&, GraphResource declaration, std::uint32_t projection = 0
        ) noexcept = 0;

        [[nodiscard]] virtual RenderResult<GraphRasterTextureSetId>
        importRasterTextures(std::string_view binding) noexcept = 0;
        [[nodiscard]] virtual RenderResult<std::optional<GraphRasterTextureSetId>>
        importOptionalRasterTextures(std::string_view binding) noexcept = 0;
        [[nodiscard]] virtual RenderResult<GraphRasterMeshSetId>
        importRasterMeshes(std::string_view binding) noexcept = 0;
        [[nodiscard]] virtual RenderResult<GraphRasterMaterialSetId>
        importRasterMaterials(std::string_view binding) noexcept = 0;

        // A preparation owned by this node supplies the initial contents of each frame's Buffer.
        [[nodiscard]] virtual RenderResult<ResourceVersion> declarePreparedBuffer(
            std::string_view name, std::uint64_t minimum_bytes, std::uint64_t maximum_bytes,
            EGraphUse initial_use = EGraphUse::STORAGE_READ
        ) noexcept = 0;

        // Preparation chooses the size. The contents are defined by graph writes each frame.
        [[nodiscard]] virtual RenderResult<ResourceVersion>
        declareFrameBuffer(std::string_view name, std::uint64_t minimum_bytes, std::uint64_t maximum_bytes) noexcept = 0;

        // Copy the complete typed member into this pass's inline range when the Job prepares a frame.
        template <ShaderConstantBlock T, class State, T State::*Member>
        [[nodiscard]] RenderResult<void> copyInlineBlock(GraphPassId pass) noexcept
        {
            return declareInlineCopy(pass, {
                cxx::make_basic_type_info<State>(), kConstantBlockContract<T>,
                [](const void* owner) noexcept
                {
                    return std::as_bytes(std::span{&(static_cast<const State*>(owner)->*Member), 1});
                }
            });
        }

        // The returned version must be used as one fixed typed uniform descriptor.
        template <ShaderConstantBlock T, class State, EffectUniformBlock<T> State::*Member>
        [[nodiscard]] RenderResult<ResourceVersion> uniformBlock(std::string_view name) noexcept
        {
            return declareUniformBlock(name, {
                cxx::make_basic_type_info<State>(),
                kConstantBlockContract<T>,
                [](const void* state) noexcept -> extension_detail::UniformSnapshot
                {
                    const auto& block = static_cast<const State*>(state)->*Member;
                    return {std::as_bytes(std::span{&block.value(), 1}), block.revision()};
                }
            });
        }

        template <CpuPreparationValue T>
        [[nodiscard]] RenderResult<GraphCpuDataId> declareCpuData() noexcept
        {
            return declareCpuValue(cxx::make_basic_type_info<T>(), sizeof(T));
        }

        template <class T>
        [[nodiscard]] RenderResult<GraphStorageSnapshotId>
        importStorageSnapshot(std::string_view binding, const ResourceRequirement& required) noexcept
        {
            return importStorageValue(binding, required, cxx::make_basic_type_info<T>());
        }

        [[nodiscard]] virtual RenderResult<void> addPreparation(GraphPreparation) noexcept = 0;

        [[nodiscard]] virtual std::span<const EffectResourceBundle> inputs(graph::PinSemanticId) const noexcept = 0;
        [[nodiscard]] virtual bool outputDemanded(graph::PinSemanticId) const noexcept = 0;
        [[nodiscard]] virtual RenderResult<void> publish(graph::PinSemanticId, EffectResourceBundle) noexcept = 0;

    private:
        [[nodiscard]] virtual const ConfigValue* nodeConfiguration() const noexcept = 0;
        [[nodiscard]] virtual RenderResult<ResourceVersion>
        declareUniformBlock(std::string_view, extension_detail::EffectUniformSource) noexcept = 0;
        [[nodiscard]] virtual RenderResult<void>
        declareInlineCopy(GraphPassId, extension_detail::InlineBlockInput) noexcept = 0;
        [[nodiscard]] virtual RenderResult<GraphCpuDataId> declareCpuValue(cxx::basic_type_info, std::size_t) noexcept = 0;
        [[nodiscard]] virtual RenderResult<GraphStorageSnapshotId>
        importStorageValue(std::string_view, const ResourceRequirement&, cxx::basic_type_info) noexcept = 0;
    };
}
