#include <lux/engine/scene/render/CameraInputOperation.hpp>
#include <lux/engine/scene/render/CameraParameters.hpp>

namespace lux::scene
{
    using namespace lux::render;
    using namespace simulation::ecs;
    using namespace detail;

    std::span<const RenderPayloadContract> CameraInputOperation::inputs() noexcept
    {
        static const auto contracts = []
        {
            RenderPayloadContract input;
            input.schema = kCameraInput;
            input.use = ERenderPayloadUse::FRAME_INPUT;
            input.lifetime = ERenderPayloadLifetime::MOVED_VALUE;
            input.required = false; // Color-only capabilities still need Target activation and viewport updates.
            input.setCppType(cxx::typeToken<CameraInput>());
            return std::array{input};
        }();
        return contracts;
    }

    CameraInputOperation::CameraInputOperation(
        RenderBindingContext& context,
        engine::render::BoundCommand<UpdateAttachmentInputs> update,
        engine::render::BoundCommand<SetTargetActive> active
    ) noexcept
        : registry_(context.registry), client_(context.client), update_(update), active_(active),
          camera_input_(std::ranges::any_of(
              context.contracts,
              [](const auto& value) noexcept { return value.cppType() == cxx::typeToken<CameraInput>(); }
          )),
          clipping_(context.effect_inputs != nullptr), camera_(context.view_entity), view_(context.view)
    {
    }

    RenderResult<std::unique_ptr<CameraInputOperation>> CameraInputOperation::create(RenderBindingContext& context
    ) noexcept
    {
        auto update = context.client.bind<UpdateAttachmentInputs>();
        auto active = context.client.bind<SetTargetActive>();
        const bool has_commands = update && active;
        const bool has_view = !context.view.isNull() && context.view_entity != NullEntity;
        if (!has_commands || !has_view)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        return std::unique_ptr<CameraInputOperation>{new CameraInputOperation(context, *update, *active)};
    }

    CameraRenderTarget* CameraInputOperation::current() const noexcept
    {
        auto* resource = registry_.valid(camera_) ? registry_.try_get<CameraRenderResourceComponent>(camera_) : nullptr;
        if (!resource)
        {
            return nullptr;
        }
        const auto found = std::ranges::find(resource->bindings, view_, &CameraRenderTarget::view);
        return found == resource->bindings.end() ? nullptr : &*found;
    }

    SceneStageResult CameraInputOperation::maintain(SceneStageContext& context) noexcept
    {
        if (context.stop.stop_requested())
        {
            changed_ = prepared_ = false;
        }
        else
        {
            context.publication_needed |= changed_ || resynchronization_ != client_.resynchronization();
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult CameraInputOperation::prepare(SceneStageContext&) noexcept
    {
        prepared_ = false;
        auto* binding = current();
        if (!binding)
        {
            return ESceneProgress::COMPLETE;
        }
        if (resynchronization_ != client_.resynchronization())
        {
            resynchronization_ = client_.resynchronization();
            changed_ = binding->parameters_changed = binding->output_changed = true;
        }
        if (!changed_)
        {
            return ESceneProgress::COMPLETE;
        }
        const bool available = registry_.all_of<Camera, CameraTargetsComponent, WorldTransform3D>(camera_);
        if (!available || binding->attachment.isNull())
        {
            return ESceneProgress::COMPLETE;
        }
        const auto& camera = registry_.get<Camera>(camera_);
        const auto& targets = registry_.get<CameraTargetsComponent>(camera_).targets;
        const auto target = std::ranges::find(targets, binding->id, &CameraTarget::id);
        if (target == targets.end() || binding->graph_inputs != cameraGraphInputs(camera, *target, clipping_))
        {
            return ESceneProgress::COMPLETE;
        }
        RenderResult<CameraInput> parameters{CameraInput{}};
        if (camera_input_ || clipping_)
        {
            auto pose = prepareSceneCamera(camera, registry_.get<WorldTransform3D>(camera_).value);
            parameters = pose ? sceneParameters(camera, *pose, *target) : cxx::unexpected(pose.error());
        }
        if (!parameters)
        {
            binding->error = parameters.error();
            binding->adopted.active = false;
            binding->output_changed = true;
        }
        else
        {
            binding->parameters_changed |=
                !sameParameters(binding->parameters, *parameters) || binding->adopted.viewport != target->viewport;
            binding->output_changed |=
                binding->adopted.active != target->active || binding->adopted.epoch != target->epoch;
            binding->parameters = *parameters;
            binding->adopted = *target;
            binding->error = {};
        }
        prepared_ = binding->parameters_changed || binding->output_changed;
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult CameraInputOperation::publish(SceneStageContext&, engine::render::FrameInput& frame) noexcept
    {
        if (prepared_)
        {
            // Structural mutation is excluded between prepare and publication.
            auto& binding = *current();
            if (binding.parameters_changed && binding.error.type == 0)
            {
                const auto bytes = std::as_bytes(std::span{&binding.parameters, 1});
                AttachmentInputs input{
                    binding.attachment,
                    camera_input_ ? std::vector<std::byte>{bytes.begin(), bytes.end()} : std::vector<std::byte>{},
                    binding.adopted.viewport
                };
                input.parameter_type = camera_input_ ? cxx::typeToken<CameraInput>() : cxx::TypeToken{};
                auto result = client_.append(frame, update_, std::move(input));
                if (!result)
                {
                    return cxx::unexpected(
                        SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, result.error()}
                    );
                }
                binding.parameters_changed = false;
            }
            if (binding.output_changed)
            {
                SetTargetActive::Request input{binding.adopted.target, binding.adopted.active, binding.adopted.epoch};
                auto result = client_.append(frame, active_, input);
                if (!result)
                {
                    return cxx::unexpected(
                        SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, result.error()}
                    );
                }
                binding.output_changed = false;
            }
        }
        changed_ = prepared_ = false;
        return ESceneProgress::COMPLETE;
    }
} // namespace lux::scene
