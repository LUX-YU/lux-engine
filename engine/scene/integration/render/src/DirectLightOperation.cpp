#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/scene/render/DirectLightBinding.hpp>
#include <lux/engine/scene/render/DirectLightOperation.hpp>
#include <lux/engine/scene/render/DirectLightValues.hpp>

namespace lux::scene
{
    using namespace lux::render;
    using namespace simulation::ecs;

    std::span<const RenderPayloadContract> DirectLightOperation::inputs() noexcept
    {
        return DirectLightBinding::inputs();
    }

    RenderResult<std::unique_ptr<DirectLightOperation>> DirectLightOperation::create(RenderBindingContext& context
    ) noexcept
    {
        const auto input = context.input(kDirectLightsInput);
        auto update = context.client.bind<UpdateDirectLights>();
        if (!input || !update)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        return std::unique_ptr<DirectLightOperation>{new DirectLightOperation(context, *input, *update)};
    }

    DirectLightOperation::DirectLightOperation(
        RenderBindingContext& context,
        CapabilityInput input,
        engine::render::BoundCommand<UpdateDirectLights> update
    ) noexcept
        : registry_(context.registry), client_(context.client), input_(input), update_(update)
    {
    }

    SceneStageResult DirectLightOperation::maintain(SceneStageContext& context) noexcept
    {
        if (context.stop.stop_requested())
        {
            changed_.clear();
            prepared_.clear();
        }
        else
        {
            context.publication_needed |= !changed_.empty() || resynchronization_ != client_.resynchronization();
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult DirectLightOperation::prepare(SceneStageContext&) noexcept
    {
        if (resynchronization_ != client_.resynchronization())
        {
            resynchronization_ = client_.resynchronization();
            for (const auto entity : registry_.view<detail::LightRenderResourceComponent>())
            {
                changed_.insert(entity);
            }
        }
        prepared_.clear();
        for (auto entry = changed_.begin(); entry != changed_.end();)
        {
            const auto entity = *entry;
            const bool available =
                registry_.valid(entity) &&
                registry_.all_of<Light3D, WorldTransform3D, detail::LightRenderResourceComponent>(entity);
            if (!available)
            {
                entry = changed_.erase(entry);
                continue;
            }
            const auto& source = registry_.get<Light3D>(entity).value;
            const auto& resource = registry_.get<detail::LightRenderResourceComponent>(entity);
            if (source.type != resource.type)
            {
                entry = changed_.erase(entry); // A successful replacement emits a new result-component event.
                continue;
            }
            prepared_.push_back(entity);
            ++entry;
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult DirectLightOperation::publish(SceneStageContext&, engine::render::FrameInput& frame) noexcept
    {
        if (!prepared_.empty())
        {
            auto updates = frame.allocate<DirectLightUpdate>(prepared_.size());
            for (std::size_t i = 0; i < prepared_.size(); ++i)
            {
                const auto entity = prepared_[i];
                auto value = detail::lightValue(
                    registry_.get<Light3D>(entity).value,
                    registry_.get<WorldTransform3D>(entity).value
                );
                if (!value)
                {
                    return cxx::unexpected(
                        SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, value.error()}
                    );
                }
                updates[i] = {registry_.get<detail::LightRenderResourceComponent>(entity).light, std::move(*value)};
            }
            const UpdateDirectLights::Request request{input_, ++sequence_, updates};
            const auto appended = client_.append(frame, update_, request);
            if (!appended)
            {
                return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, appended.error()}
                );
            }
        }
        for (const auto entity : prepared_)
        {
            changed_.erase(entity);
        }
        prepared_.clear();
        return ESceneProgress::COMPLETE;
    }
} // namespace lux::scene
