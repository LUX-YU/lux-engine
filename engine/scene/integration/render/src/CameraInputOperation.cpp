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
          clipping_(context.effect_inputs != nullptr)
    {
    }

    RenderResult<std::unique_ptr<CameraInputOperation>> CameraInputOperation::create(RenderBindingContext& context
    ) noexcept
    {
        auto update = context.client.bind<UpdateAttachmentInputs>();
        auto active = context.client.bind<SetTargetActive>();
        if (!update || !active)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        return std::unique_ptr<CameraInputOperation>{new CameraInputOperation(context, *update, *active)};
    }

    SceneStageResult CameraInputOperation::maintain(SceneStageContext& context) noexcept
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

    SceneStageResult CameraInputOperation::prepare(SceneStageContext&) noexcept
    {
        if (resynchronization_ != client_.resynchronization())
        {
            resynchronization_ = client_.resynchronization();
            for (const auto entity : registry_.view<CameraRenderResourceComponent>())
            {
                changed_.insert(entity);
                for (auto& binding : registry_.get<CameraRenderResourceComponent>(entity).bindings)
                {
                    binding.parameters_changed = true;
                    binding.output_changed = true;
                }
            }
        }
        prepared_.clear();
        for (auto entry = changed_.begin(); entry != changed_.end();)
        {
            const auto entity = *entry;
            const bool available =
                registry_.valid(entity) &&
                registry_.all_of<Camera, CameraTargetsComponent, CameraRenderResourceComponent, WorldTransform3D>(entity
                );
            if (!available)
            {
                entry = changed_.erase(entry);
                continue;
            }
            const auto& camera = registry_.get<Camera>(entity);
            const bool needs_parameters = camera_input_ || clipping_;
            auto prepared = needs_parameters ? prepareSceneCamera(camera, registry_.get<WorldTransform3D>(entity).value)
                                             : RenderResult<PreparedCamera>{PreparedCamera{}};
            auto& bindings = registry_.get<CameraRenderResourceComponent>(entity).bindings;
            const auto& targets = registry_.get<CameraTargetsComponent>(entity).targets;
            for (auto& binding : bindings)
            {
                if (binding.attachment.isNull())
                {
                    continue;
                }
                const auto target = std::ranges::find(targets, binding.id, &CameraTarget::id);
                if (target == targets.end())
                {
                    continue;
                }
                if (binding.graph_inputs != cameraGraphInputs(camera, *target, clipping_))
                {
                    continue;
                }
                auto parameters = !needs_parameters ? RenderResult<CameraInput>{CameraInput{}}
                                  : prepared        ? sceneParameters(camera, *prepared, *target)
                                                    : RenderResult<CameraInput>{cxx::unexpected(prepared.error())};
                if (!parameters)
                {
                    binding.error = parameters.error();
                    binding.adopted.active = false;
                    binding.output_changed = true;
                }
                else
                {
                    binding.parameters_changed |= !sameParameters(binding.parameters, *parameters) ||
                                                  binding.adopted.viewport != target->viewport;
                    binding.output_changed |=
                        binding.adopted.active != target->active || binding.adopted.epoch != target->epoch;
                    binding.parameters = *parameters;
                    binding.adopted = *target;
                    binding.error = {};
                }
                if (binding.parameters_changed || binding.output_changed)
                {
                    prepared_.emplace_back(entity, binding.id);
                }
            }
            ++entry;
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult CameraInputOperation::publish(SceneStageContext&, engine::render::FrameInput& frame) noexcept
    {
        for (const auto [entity, id] : prepared_)
        {
            auto& bindings = registry_.get<CameraRenderResourceComponent>(entity).bindings;
            auto& binding = *std::ranges::find(bindings, id, &CameraRenderTarget::id);
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
        changed_.clear();
        prepared_.clear();
        return ESceneProgress::COMPLETE;
    }
} // namespace lux::scene
