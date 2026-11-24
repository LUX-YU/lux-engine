#pragma once

#include <lux/engine/render/visibility.h>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/graph/ConstantBlock.hpp>
#include <lux/engine/render/effect/FrameAlgorithm.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <lux/engine/render/graph/Work.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/ResourceProjection.hpp>
#include <lux/engine/render/graph/GraphProgram.hpp>
#include <lux/engine/render/graph/Sampler.hpp>
#include <lux/cxx/core/function_ref.hpp>
#include <array>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace lux::render
{
    namespace runtime_detail { struct EffectLoweringWorkspace; struct EffectResourceOperand; }
    namespace effect_detail { struct ValidatedEffectNode; struct ValidatedEffectPin; }

    struct EffectResourceValue final
    {
        graph::PinSemanticId member;
        ResourceVersion version;
    };

    using EffectResourceBundle = std::vector<EffectResourceValue>;

    struct ResourceBufferExtent final
    {
        std::string_view requirement;
        std::uint32_t field;
    };

    struct EffectBuildDevice final
    {
        rdesc::EShaderTarget shader_target;
        bool buffer_device_address;
        bool indirect_first_instance;
        std::array<std::uint32_t, 3> compute_dispatch_limit;
        std::uint64_t storage_buffer_max_range;
        std::uint32_t texture_layer_limit;
        bool texture_descriptors = false;
        bool record_table = false;
    };

    // Short-lived contribution context for one device/target variant. Bundles hold graph versions
    // which remain valid only for this build. The context never exposes mutable logical state.
    class LUX_RENDER_PUBLIC EffectCompileContext final
    {
    public:
        EffectCompileContext(const EffectCompileContext&) = delete;
        EffectCompileContext& operator=(const EffectCompileContext&) = delete;
        [[nodiscard]] graph::NodeId nodeId() const noexcept;
        // Borrows the immutable value published during CreateEffect for this build call.
        // Extents are evaluated per instance from graph declarations, including through compatible resize.
        [[nodiscard]] const GraphOutputRequirements& target() const noexcept;
        [[nodiscard]] const EffectBuildDevice& device() const noexcept;
        [[nodiscard]] rdesc::EShaderTarget shaderTarget() const noexcept { return device().shader_target; }
        [[nodiscard]] const VGraphResourceDesc* resourceDescription(GraphResourceId) const & noexcept;
        const VGraphResourceDesc* resourceDescription(GraphResourceId) const && = delete;

        // Validates the logical description, including imported initial/final access contracts.
        [[nodiscard]] RenderResult<GraphResourceId> declareResource(GraphResource resource) noexcept;
        [[nodiscard]] RenderResult<HistoryDescription> declareHistory(std::string key, HistoryDeclaration declaration) noexcept;
        [[nodiscard]] RenderResult<void> publishHistory(ResourceVersion, std::optional<PassCondition> when = std::nullopt) noexcept;
        [[nodiscard]] RenderResult<PassCondition> sameBinding(GraphResourceId left, GraphResourceId right) noexcept;
        template <class First, class Second>
            requires(std::is_nothrow_invocable_r_v<RenderResult<ResourceVersion>, First&, EffectCompileContext&> &&
                     std::is_nothrow_invocable_r_v<RenderResult<ResourceVersion>, Second&, EffectCompileContext&>)
        [[nodiscard]] RenderResult<ResourceVersion>
        choose(PassCondition predicate, First&& when_true, Second&& when_false) noexcept
        {
            return declareChoice(predicate, when_true, when_false);
        }
        [[nodiscard]] RenderResult<std::uint32_t> declareCondition(std::string key) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> addProgram(GraphProgram program) noexcept;
        [[nodiscard]] std::uint32_t addSampler(SamplerDescription description) noexcept;

        // Pass resource accesses use declared ranges/versions. Writes return new versions in PassResult;
        // callers connect consumers to those versions instead of inventing execution ordering.
        [[nodiscard]] RenderResult<PassResult>
        addRasterPass(std::string key, RasterWork work, GraphPassOptions = GraphPassOptions{}) noexcept;
        [[nodiscard]] RenderResult<PassResult>
        addComputePass(std::string key, ComputeWork work, GraphPassOptions = GraphPassOptions{}) noexcept;
        // Commands run in order on one queue. The result exposes each resource's last write.
        // Use nextWriteVersion to declare a read of an earlier command's output.
        [[nodiscard]] RenderResult<PassResult>
        addComputeSequence(std::string key, std::vector<ComputeWork> commands, GraphPassOptions = GraphPassOptions{}) noexcept;
        [[nodiscard]] RenderResult<ResourceVersion> nextWriteVersion(GraphResourceId resource) const noexcept;
        [[nodiscard]] RenderResult<PassResult>
        addTransferPass(std::string key, TransferWork work, GraphPassOptions = GraphPassOptions{}) noexcept;
        // Duplicate identical edges are canonicalized. Self edges are errors.
        [[nodiscard]] RenderResult<void> dependsOn(GraphPassId before, GraphPassId after) noexcept;

        // Read-only sampling of an explicitly bound, same-device offscreen Target.
        [[nodiscard]] RenderResult<ResourceVersion> importTargetImage(
            std::string_view binding, rdesc::ETextureFormat format, std::uint32_t samples = 1
        ) noexcept;
        [[nodiscard]] RenderResult<ResourceVersion>
        importResource(
            std::string_view binding,
            ResourceNameView owner,
            ResourceProjection declaration,
            std::uint32_t projection = 0
        ) noexcept;

        // CPU views and GPU projections use the same scoped Resource requirement.
        [[nodiscard]] RenderResult<ResourceVersion>
        importProjection(
            const ResourceRequirement& requirement,
            std::string_view operand,
            ResourceProjection declaration,
            std::uint32_t projection = 0
        ) noexcept;

        [[nodiscard]] RenderResult<ResourceVersion>
        importFinalBuffer(
            std::string_view binding, ResourceNameView owner, GraphResource declaration,
            std::uint32_t projection = 0
        ) noexcept;

        // A preparation owned by this node supplies the initial contents of each frame's Buffer.
        [[nodiscard]] RenderResult<ResourceVersion> declarePreparedBuffer(
            std::string_view name, std::uint64_t minimum_bytes, std::uint64_t maximum_bytes,
            EGraphUse initial_use = EGraphUse::STORAGE_READ
        ) noexcept;

        // Graph writes define the contents. A resource extent supplies the admitted FIF byte count;
        // without it, a CPU algorithm must explicitly publish the frame size.
        [[nodiscard]] RenderResult<ResourceVersion>
        declareFrameBuffer(
            std::string_view name, std::uint64_t minimum_bytes, std::uint64_t maximum_bytes,
            std::optional<ResourceBufferExtent> extent = {}
        ) noexcept;

        // Copy the complete typed member into this pass's inline range when the Job prepares a frame.
        template <ShaderConstantBlock T, class State, T State::*Member>
        [[nodiscard]] RenderResult<void> copyInlineBlock(GraphPassId pass) noexcept
        {
            return declareInlineCopy(pass, {
                cxx::make_basic_type_info<State>(), kConstantBlockContract<T>,
                [](const void* owner) noexcept
                {
                    const auto& state = *static_cast<const State*>(owner);
                    return std::as_bytes(std::span{&(state.*Member), 1});
                }
            });
        }

        // The returned version must be used as one fixed typed uniform descriptor.
        template <ShaderConstantBlock T, class State, T State::*Value, std::uint64_t State::*Revision>
        [[nodiscard]] RenderResult<ResourceVersion> constantBuffer() noexcept
        {
            return declareConstantBuffer({
                cxx::make_basic_type_info<State>(), kConstantBlockContract<T>,
                [](const void* owner) noexcept -> extension_detail::BoundConstantValue
                {
                    const auto& state = *static_cast<const State*>(owner);
                    return {std::as_bytes(std::span{&(state.*Value), 1}), &(state.*Revision)};
                }
            });
        }

        // A CPU view and GPU projections can select the same scoped Resource under distinct operand names.
        template <class T> [[nodiscard]] RenderResult<ResourceViewSlot>
        readResourceView(const ResourceRequirement& source, std::string_view operand) noexcept
        {
            return bindResourceView(source, operand, cxx::make_basic_type_info<T>());
        }

        template <auto Frame, auto Link = nullptr, auto Create = nullptr>
        [[nodiscard]] RenderResult<void>
        onFrame(typename extension_detail::FrameBinding<Frame, Link, Create>::Configuration config, FrameAlgorithmUses uses) noexcept
        {
            auto owned = ConfigValue::make(std::move(config));
            if (!owned) return cxx::unexpected(owned.error());
            using Binding = extension_detail::FrameBinding<Frame, Link, Create>;
            return declareFrameAlgorithm(std::move(uses), std::move(*owned), &Binding::bind);
        }

        // The connected output bundles remain owned by this compilation. Read them during the build callback.
        [[nodiscard]] auto inputs(graph::PinSemanticId semantic) const noexcept
        {
            return sourcePins(semantic) | std::views::transform(
                [bundles = sourceBundles()](std::uint32_t pin) noexcept -> const EffectResourceBundle& { return bundles[pin]; }
            );
        }
        [[nodiscard]] bool outputDemanded(graph::PinSemanticId) const noexcept;
        [[nodiscard]] RenderResult<void> publish(graph::PinSemanticId, EffectResourceBundle) noexcept;

    private:
        friend struct runtime_detail::EffectLoweringWorkspace;
        RenderResult<void> declareFrameAlgorithm(FrameAlgorithmUses, ConfigValue, extension_detail::BindFrameAlgorithm) noexcept;

        EffectCompileContext(runtime_detail::EffectLoweringWorkspace&, const effect_detail::ValidatedEffectNode&) noexcept;
        [[nodiscard]] std::span<const std::uint32_t> sourcePins(graph::PinSemanticId) const noexcept;
        [[nodiscard]] std::span<const EffectResourceBundle> sourceBundles() const noexcept;
        using ChoiceArm = cxx::function_ref<RenderResult<ResourceVersion>(EffectCompileContext&) noexcept>;
        [[nodiscard]] RenderResult<ResourceVersion> declareChoice(PassCondition, ChoiceArm, ChoiceArm) noexcept;
        [[nodiscard]] RenderResult<ResourceVersion>
        declarePreparationBuffer(std::string_view, std::uint64_t, std::uint64_t, EGraphUse) noexcept;
        [[nodiscard]] std::size_t portIndex(graph::PinSemanticId) const noexcept;
        [[nodiscard]] bool complete() const noexcept;

        runtime_detail::EffectLoweringWorkspace& workspace_;
        const effect_detail::ValidatedEffectNode& node_;
        std::span<const effect_detail::ValidatedEffectPin> pins_;
        std::span<EffectResourceBundle> outputs_;

        [[nodiscard]] RenderResult<std::uint32_t> requireResource(std::string_view binding) noexcept;
        [[nodiscard]] RenderResult<ResourceVersion>
        bindStorageProjection(runtime_detail::EffectResourceOperand, std::optional<GraphBufferRange>) noexcept;
        [[nodiscard]] RenderResult<ResourceVersion>
        declareConstantBuffer(extension_detail::ConstantBufferField) noexcept;
        [[nodiscard]] RenderResult<void>
        declareInlineCopy(GraphPassId, extension_detail::InlineBlockInput) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> linkRequirement(const ResourceRequirement&) noexcept;
        [[nodiscard]] RenderResult<ResourceViewSlot>
        bindResourceView(const ResourceRequirement&, std::string_view operand, cxx::basic_type_info) noexcept;
    };
}
