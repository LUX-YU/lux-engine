#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/scene/render/RasterTransformOperation.hpp>

namespace lux::scene
{
    using namespace lux::render;
    using namespace simulation::ecs;

    std::span<const RenderPayloadContract> RasterTransformOperation::inputs() noexcept
    {
        return MeshRenderBinding::inputs().first(1);
    }

    RenderResult<std::unique_ptr<RasterTransformOperation>> RasterTransformOperation::create(
        RenderBindingContext& context
    ) noexcept
    {
        auto update = context.client.bind<UpdateTransforms>();
        const auto input =
            std::ranges::find(context.inputs, kRasterInstancesInput, &CapabilitiesInstalled::Input::schema);
        if (!update || input == context.inputs.end())
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        auto result =
            std::unique_ptr<RasterTransformOperation>{new RasterTransformOperation(context, input->destination, *update)
            };
        return result;
    }

    SceneStageResult RasterTransformOperation::maintain(SceneStageContext& context) noexcept
    {
        if (context.stop.stop_requested())
        {
            changed_.clear();
            prepared_.clear();
        }
        else
        {
            context.publication_needed |= !changed_.empty() || client_.resynchronization() != resynchronization_;
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult RasterTransformOperation::prepare(SceneStageContext&) noexcept
    {
        if (resynchronization_ != client_.resynchronization())
        {
            resynchronization_ = client_.resynchronization();
            for (const auto entity : registry_.view<detail::MeshInstanceRenderResourceComponent>())
            {
                changed_.insert(entity);
            }
        }
        prepared_.clear();
        for (auto entry = changed_.begin(); entry != changed_.end();)
        {
            if (!registry_.valid(*entry) ||
                !registry_.all_of<detail::MeshInstanceRenderResourceComponent, WorldTransform3D>(*entry))
            {
                entry = changed_.erase(entry);
                continue;
            }
            prepared_.push_back(*entry++);
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult RasterTransformOperation::publish(SceneStageContext&, engine::render::FrameInput& frame) noexcept
    {
        if (prepared_.empty())
        {
            return ESceneProgress::COMPLETE;
        }
        auto ids = frame.allocate<MeshInstanceId>(prepared_.size());
        auto matrices = frame.allocate<TransformMatrix>(prepared_.size());
        for (std::size_t i = 0; i < prepared_.size(); ++i)
        {
            ids[i] = registry_.get<detail::MeshInstanceRenderResourceComponent>(prepared_[i]).instance;
            Eigen::Map<Eigen::Matrix4f>{matrices[i].data()} =
                registry_.get<WorldTransform3D>(prepared_[i]).value.matrix().cast<float>();
        }
        const TransformBatch request{input_, ++sequence_, ids, matrices};
        auto appended = client_.append(frame, update_, request);
        if (!appended)
        {
            return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, appended.error()});
        }
        for (const auto entity : prepared_)
        {
            changed_.erase(entity);
        }
        prepared_.clear();
        return ESceneProgress::COMPLETE;
    }
} // namespace lux::scene
