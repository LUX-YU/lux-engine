#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/runtime/Scene.hpp>

namespace lux::render::runtime_detail
{
    detail::TargetOutput RenderTarget::output() const noexcept
    {
        return std::visit([](const auto& value) noexcept { return value->output(); }, physical);
    }

    detail::SurfaceTarget* RenderTarget::surface() const noexcept
    {
        const auto* value = std::get_if<std::unique_ptr<detail::SurfaceTarget>>(&physical);
        return value ? value->get() : nullptr;
    }

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
            target_images = std::move(other.target_images);
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
            instance->plan().resources()[resource].description,
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

} // namespace lux::render::runtime_detail
