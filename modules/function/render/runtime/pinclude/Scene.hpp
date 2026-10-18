#pragma once

#include <list>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/resource/Binding.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <memory>
#include <vector>

namespace lux::render::vulkan
{
    class CompiledGraph;
    class ExecutionContext;
} // namespace lux::render::vulkan

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
        std::uint64_t epoch = 1;
        std::list<AttachmentId> attachments;

        explicit RenderTarget(TargetDesc value = {}) noexcept : description(value) {}

        RenderTarget(const RenderTarget&) = delete;
        RenderTarget& operator=(const RenderTarget&) = delete;
    };

    struct RenderEffect final
    {
        std::shared_ptr<const vulkan::CompiledGraph> code;
        std::vector<StorageArgument> arguments;
        std::vector<AttachmentId> attachments;
    };

    struct EffectAttachment final
    {
        EffectId effect;
        SceneId scene;
        ViewId view;
        TargetId target;
        // DeviceSession currently retains execution storage through its real GPU retirement path.
        vulkan::ExecutionContext* context;
        std::size_t view_position = 0;
        std::size_t effect_position = 0;
        std::list<AttachmentId>::iterator target_position;

        void link(AttachmentId id, RenderView& render_view, RenderEffect& render_effect, RenderTarget& output) noexcept
        {
            view_position = render_view.attachments.size();
            effect_position = render_effect.attachments.size();
            render_view.attachments.push_back(id);
            render_effect.attachments.push_back(id);
            target_position = output.attachments.insert(output.attachments.end(), id);
        }

        void unlink(
            cxx::SlotMap<EffectAttachment, AttachmentTag>& attachments,
            RenderView& render_view,
            RenderEffect& render_effect,
            RenderTarget& output
        ) noexcept
        {
            const auto last_view = render_view.attachments.back();
            render_view.attachments[view_position] = last_view;
            attachments.find(last_view)->view_position = view_position;
            render_view.attachments.pop_back();

            const auto last_effect = render_effect.attachments.back();
            render_effect.attachments[effect_position] = last_effect;
            attachments.find(last_effect)->effect_position = effect_position;
            render_effect.attachments.pop_back();
            output.attachments.erase(target_position);
        }
    };
} // namespace lux::render::runtime_detail
