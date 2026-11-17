#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/execution/SurfaceTarget.hpp>
#include <lux/engine/render/extensions/CapabilityImplementation.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <lux/engine/render/extensions/features/FeatureBinding.hpp>
#include <lux/engine/render/extensions/graph/Sampler.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/extensions/resource/Binding.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <memory>
#include <vector>

namespace lux::render::detail
{
    class RenderPlan;
    class RenderGraphInstance;
} // namespace lux::render::detail

namespace lux::render::runtime_detail
{
    struct RenderView final
    {
        ResourceSet storages;
        std::vector<AttachmentId> attachments;
        CameraInput camera;
        std::uint64_t camera_revision = 1;

        explicit RenderView(ResourceRegistry& registry, ResourceSet& parent) noexcept
            : storages(registry, EResourceDomain::VIEW, &parent) {}
    };

    struct RenderScene final
    {
        ResourceSet storages;
        cxx::StableSlotMap<RenderView, ViewTag> views;

        explicit RenderScene(ResourceRegistry& registry, ResourceSet& parent) noexcept
            : storages(registry, EResourceDomain::SCENE, &parent) {}
    };

    struct RenderTarget final
    {
        using VPhysicalTarget =
            std::variant<std::unique_ptr<detail::OffscreenTarget>, std::unique_ptr<detail::SurfaceTarget>>;
        TargetDesc description;
        VPhysicalTarget physical;
        std::uint64_t epoch = 1;
        AttachmentId attachment;
        std::vector<AttachmentId> consumers; // Non-owning sampled output relationships.
        bool enabled = true;
        bool resizing = false; // One accepted structural transaction, not another physical resource owner.

        explicit RenderTarget(VPhysicalTarget value) noexcept : physical(std::move(value))
        {
            description = output().description;
        }

        [[nodiscard]] detail::TargetOutput output() const noexcept;
        [[nodiscard]] detail::SurfaceTarget* surface() const noexcept;

        RenderTarget(const RenderTarget&) = delete;
        RenderTarget& operator=(const RenderTarget&) = delete;
    };

    struct RenderEffect final
    {
        RenderGraph graph;
        RenderGraphAnalysis analysis;
        EffectOutputDesc output;
        std::vector<std::uint32_t> parameter_groups;
        std::uint32_t pending_executions = 0; // Native candidates borrow this immutable recipe until delivery.

        // One cold-resolved dependency used by both public upload inputs and Graph resource bindings.
        // These records identify declarations and graph bindings only; the actual instances remain in the owning ResourceSets.
        struct ResourceDependency final
        {
            EResourceDomain scope;
            ResourceTypeId declaration;
            std::vector<StorageArgument> bindings;

            // Pointees belong to the retained domain; call metadata lives in stable Resource type slots.
            // No pointer into a relocatable instance record or Client payload is retained here.
            struct Input final
            {
                std::uint32_t capability = 0;
                std::uint32_t index = 0;
            };

            std::vector<Input> inputs;
        };

        std::vector<ResourceDependency> resource_dependencies;
        std::vector<detail::FeatureFrameContribution> bindings;
        std::vector<SamplerDescription> samplers;
        std::vector<AttachmentId> attachments;
        struct ViewProjection final
        {
            cxx::type_info type;
            FrameParameterProjection apply;
            std::uint32_t parameter_group;
        };
        std::vector<ViewProjection> view_projections;

        struct Implementation final
        {
            CapabilityImplementationId id;
            std::string contribution;

            std::vector<std::uint32_t> resource_dependencies;
        };

        std::vector<Implementation> implementations;
        std::vector<GraphPassId> draw_inputs;
    };

    struct EffectAttachment final
    {
        EffectAttachment() noexcept;
        ~EffectAttachment() noexcept;
        EffectAttachment(EffectAttachment&&) noexcept;
        EffectAttachment& operator=(EffectAttachment&&) noexcept;

        EffectId effect;
        SceneId scene;
        ViewId view;
        TargetId target;
        FrameLayout input_layout;
        // Pointees belong to the retained domain; call metadata lives in stable Resource type slots.
            // No pointer into a relocatable instance record or Client payload is retained here.
            struct Input final
        {
            ResourceInstanceId identity;
            void* object;
            const FrameDataCall* call;
            std::vector<std::uint32_t> columns;
        };
        std::vector<Input> inputs;
        // The instance borrows its execution-specific native plan, never an Effect-owned native object.
        std::unique_ptr<detail::RenderPlan> plan;
        std::vector<ParameterProjection> view_projections;
        std::unique_ptr<detail::RenderGraphInstance> instance;

        struct StorageInput final
        {
            detail::RenderGraphInstance* instance;
            std::uint32_t resource;
            detail::StorageSubscription subscription;
        };

        // Destruction disconnects professional data and inputs before the graph instance.
        std::vector<StorageInput> storage_inputs;
        std::vector<detail::FeatureBinding> feature_bindings;

        struct TargetImage final
        {
            TargetId target;
            GraphResourceId resource;
        };

        std::vector<TargetImage> target_images;
        [[nodiscard]] RenderResult<void> bindStorage(
            std::uint32_t resource,
            const detail::StorageBindingOps&,
            void* native,
            const void* argument,
            std::uint32_t frame_count
        ) noexcept;
        std::uint64_t camera_revision = 0;
        std::size_t view_position = 0;
        std::size_t effect_position = 0;
    };
} // namespace lux::render::runtime_detail
