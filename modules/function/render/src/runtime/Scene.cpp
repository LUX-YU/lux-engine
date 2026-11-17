#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
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

    EffectAttachment::~EffectAttachment() noexcept
    {
        // Feature data follows Graph dependency order at construction. Consumers disconnect
        // before their producing data, and all of them precede the instance.
        while (!feature_bindings.empty())
        {
            feature_bindings.pop_back();
        }
    }

    EffectAttachment::EffectAttachment(EffectAttachment&&) noexcept = default;

    EffectAttachment& EffectAttachment::operator=(EffectAttachment&& other) noexcept
    {
        if (this != &other)
        {
            while (!feature_bindings.empty())
            {
                feature_bindings.pop_back();
            }
            storage_inputs.clear();
            instance.reset();
            plan = std::move(other.plan);
            view_projections = std::move(other.view_projections);
            instance = std::move(other.instance);
            storage_inputs = std::move(other.storage_inputs);
            feature_bindings = std::move(other.feature_bindings);
            target_images = std::move(other.target_images);
            effect = other.effect;
            scene = other.scene;
            view = other.view;
            target = other.target;
            input_layout = std::move(other.input_layout);
            inputs = std::move(other.inputs);
            camera_revision = other.camera_revision;
            view_position = other.view_position;
            effect_position = other.effect_position;
        }
        return *this;
    }

    RenderResult<void> EffectAttachment::bindStorage(
        std::uint32_t resource,
        const detail::StorageBindingOps& ops,
        void* native,
        const void* argument,
        std::uint32_t frame_count
    ) noexcept
    {
        auto& use = storage_inputs.emplace_back(instance.get(), resource);
        use.subscription.receiver = &use;
        use.subscription.changed = [](void* value, std::uint32_t slot, const detail::ResourceInput& resource) noexcept
        {
            auto& link = *static_cast<EffectAttachment::StorageInput*>(value);
            link.instance->setResource(link.resource, slot, resource);
        };
        std::array<detail::ResourceInput, 3> slices;
        auto resolved = ops.resolve(
            native,
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
