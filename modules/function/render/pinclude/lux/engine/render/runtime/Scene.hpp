#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/features/FeatureBinding.hpp>
#include <lux/engine/render/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/resource/Binding.hpp>
#include <lux/engine/render/resource/StorageSet.hpp>
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
        StorageSet storages;
        std::vector<AttachmentId> attachments;

        explicit RenderView(ResourceRegistry& registry) noexcept : storages(registry) {}
    };

    struct RenderScene final
    {
        StorageSet storages;
        cxx::StableSlotMap<RenderView, ViewTag> views;

        explicit RenderScene(ResourceRegistry& registry) noexcept : storages(registry) {}
    };

    struct RenderTarget final
    {
        TargetDesc description;
        std::unique_ptr<detail::OffscreenTarget> physical;
        std::uint64_t epoch = 1;
        AttachmentId attachment;

        explicit RenderTarget(std::unique_ptr<detail::OffscreenTarget> value) noexcept
            : description(value->description()), physical(std::move(value))
        {
        }

        RenderTarget(const RenderTarget&) = delete;
        RenderTarget& operator=(const RenderTarget&) = delete;
    };

    struct RenderEffect final
    {
        std::unique_ptr<detail::RenderPlan> plan;
        RenderGraph graph;
        RenderGraphAnalysis analysis;
        GraphStorageBindings storage_requirements;
        std::vector<StorageArgument> arguments;
        std::vector<detail::FeatureFrameContribution> bindings;
        std::vector<AttachmentId> attachments;
    };

    struct EffectAttachment final
    {
        EffectId effect;
        SceneId scene;
        ViewId view;
        TargetId target;
        std::unique_ptr<StorageSet> storages;
        std::unique_ptr<detail::RenderGraphInstance> instance;
        std::size_t view_position = 0;
        std::size_t effect_position = 0;
    };
} // namespace lux::render::runtime_detail
