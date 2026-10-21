#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/runtime/Scene.hpp>

namespace lux::render::runtime_detail
{
    EffectAttachment::EffectAttachment() noexcept = default;

    EffectAttachment::~EffectAttachment() noexcept = default;

    EffectAttachment::EffectAttachment(EffectAttachment&&) noexcept = default;

    EffectAttachment& EffectAttachment::operator=(EffectAttachment&& other) noexcept
    {
        if (this != &other)
        {
            feature_bindings.clear();
            storage_inputs.clear();
            instance.reset();
            storages = std::move(other.storages);
            instance = std::move(other.instance);
            storage_inputs = std::move(other.storage_inputs);
            feature_bindings = std::move(other.feature_bindings);
            effect = other.effect;
            scene = other.scene;
            view = other.view;
            target = other.target;
            view_position = other.view_position;
            effect_position = other.effect_position;
        }
        return *this;
    }

    RenderResult<void> EffectAttachment::bindStorage(
        std::uint32_t resource,
        StorageId id,
        const detail::StorageBindingOps& ops,
        void* native,
        const void* argument,
        std::uint32_t frame_count
    ) noexcept
    {
        auto& use = storage_inputs.emplace_back(instance.get(), id, resource);
        use.subscription.receiver = &use;
        use.subscription.changed = [](void* value, std::uint32_t slot, const detail::ResourceInput& resource) noexcept
        {
            auto& link = *static_cast<EffectAttachment::StorageInput*>(value);
            link.instance->setResource(link.resource, slot, resource);
        };
        use.ops = &ops;
        use.native = native;
        std::array<detail::ResourceInput, 3> slices;
        auto resolved = ops.resolve(
            use.native,
            instance->plan().resources()[resource],
            argument,
            use.subscription,
            std::span(slices).first(frame_count)
        );
        if (!resolved)
        {
            return cxx::unexpected(resolved.error());
        }
        for (std::uint32_t slot = 0; slot < frame_count; ++slot)
        {
            instance->setResource(resource, slot, slices[slot]);
        }
        return {};
    }

    void EffectAttachment::swapResources(EffectAttachment& candidate) noexcept
    {
        instance.swap(candidate.instance);
        storage_inputs.swap(candidate.storage_inputs);
        feature_bindings.swap(candidate.feature_bindings);
    }
} // namespace lux::render::runtime_detail
