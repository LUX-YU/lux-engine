#include <entt/container/dense_set.hpp>
#include <list>
#include <lux/engine/context/render/RenderDiagnostics.hpp>
#include <lux/engine/context/render/RenderRequester.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/scene/render/CameraParameters.hpp>
#include <lux/engine/scene/render/CameraRenderResourceComponent.hpp>
#include <lux/engine/scene/render/CameraViewBinding.hpp>

namespace lux::scene
{
    using namespace lux::render;
    using namespace engine::render;
    using namespace simulation::ecs;
    using namespace detail;

    struct CameraViewBinding::Impl final
    {
        using Commands = std::tuple<
            BoundCommand<CreateView>,
            BoundCommand<CreateEffect>,
            BoundCommand<Attach>,
            BoundCommand<Detach>,
            BoundCommand<RemoveView>,
            BoundCommand<RemoveEffect>,
            BoundCommand<SetTargetActive>>;

        struct Change
        {
            Entity camera;
            CameraTarget target;
            CameraGraphInputs graph_inputs;
            CameraInput parameters;
            ConfigValue inputs;
            ViewId view;
            EffectId effect;
            AttachmentId attachment, replaced;
            std::uint64_t input_revision = 0, effect_revision = 0;
            bool created_view = false, pending = false;
            RenderError error;
        };

        Registry& registry;
        RenderRequester& client;
        execution::ExecutionRuntime& execution;
        SceneId scene;
        CapabilitySelection implementation;
        const RenderSystemConfiguration& configuration;
        std::uint64_t& input_revision;
        std::uint64_t& adopted_revision;
        Commands commands;
        void* input_object;
        RenderResult<ConfigValue> (*compose_inputs)(void*, cxx::TypeToken, std::span<const std::byte>) noexcept;
        std::uint64_t (*recipe_revision)(void*) noexcept;
        bool camera_input_enabled;
        bool closing = false, exit_pending = false;
        std::uint64_t next_camera_target = 0, observed_inputs = 0;
        entt::dense_set<Entity> view_changes;
        std::list<Change> changes;
        std::vector<RetiringView> retiring_views;
        std::optional<RetiringView> exit_work;
        RenderError error, exit_error;

        Impl(RenderBindingContext& context, Commands selected)
            : registry(context.registry), client(context.client), execution(context.execution), scene(context.scene),
              implementation(context.implementation), configuration(context.configuration),
              input_revision(context.input_revision), adopted_revision(context.adopted_input_revision),
              commands(std::move(selected)), input_object(context.effect_input_object),
              compose_inputs(context.effect_inputs), recipe_revision(context.effect_revision),
              camera_input_enabled(std::ranges::any_of(
                  context.contracts,
                  [](const auto& value) noexcept { return value.cppType() == cxx::typeToken<CameraInput>(); }
              ))
        {
        }

        template <RenderCommand C> auto request(typename C::Request& value) noexcept
        {
            return client.request(std::get<BoundCommand<C>>(commands), value);
        }

        struct CameraTargetWaiter
        {
            CameraTargetWaiter* next{};
            Entity camera;
            CameraTargetId target;
            bool retiring{};
            void (*complete)(CameraTargetWaiter&, RenderResult<void>) noexcept {};
            std::shared_ptr<const void> code_lifetime;
        };

        CameraTargetWaiter* camera_target_waiters = nullptr;

        struct CameraTargetSender final
        {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(RenderResult<void>)>;
            Impl& system;
            Entity camera;
            CameraTargetId target;
            bool retiring;

            template <class Receiver> struct Operation final : CameraTargetWaiter
            {
                using operation_state_concept = stdexec::operation_state_t;
                Impl& system;
                Receiver receiver;

                Operation(const CameraTargetSender& sender, Receiver receiver) noexcept
                    : system(sender.system), receiver(std::move(receiver))
                {
                    this->camera = sender.camera;
                    this->target = sender.target;
                    this->retiring = sender.retiring;
                    this->code_lifetime = execution::detail::getCompletionCode(stdexec::get_env(this->receiver));
                    this->complete = [](CameraTargetWaiter& waiter, RenderResult<void> result) noexcept
                    {
                        auto& operation = static_cast<Operation&>(waiter);
                        stdexec::set_value(std::move(operation.receiver), std::move(result));
                    };
                }

                void start() & noexcept
                {
                    system.awaitCameraTarget(*this);
                }
            };

            template <class Receiver> auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{*this, std::move(receiver)};
            }
        };

        bool cameraTargetRetired(Entity camera, CameraTargetId id) const noexcept
        {
            const bool active =
                (!closing && cameraTarget(camera, id)) || cameraBinding(camera, id) ||
                std::ranges::any_of(
                    changes,
                    [&](const auto& value) noexcept { return value.camera == camera && value.target.id == id; }
                );
            const bool withdrawing = exit_work && exit_work->camera == camera && exit_work->binding.id == id;
            return !active && !withdrawing &&
                   std::ranges::none_of(
                       retiring_views,
                       [&](const auto& value) noexcept { return value.camera == camera && value.binding.id == id; }
                   );
        }

        std::optional<RenderResult<void>> cameraTargetCompletion(const CameraTargetWaiter& waiter) const noexcept
        {
            if (waiter.retiring && cameraTargetRetired(waiter.camera, waiter.target))
            {
                return RenderResult<void>{};
            }
            if (error.type != 0 &&
                (!waiter.retiring || (closing && changes.empty() && !exit_work && retiring_views.empty())))
            {
                return RenderResult<void>{cxx::unexpected(error)};
            }
            if (!waiter.retiring)
            {
                if (closing)
                {
                    return RenderResult<void>{cxx::unexpected(RenderError{kCommandCancelled})};
                }
                const auto* desired = cameraTarget(waiter.camera, waiter.target);
                const auto* binding = cameraBinding(waiter.camera, waiter.target);
                if (!desired)
                {
                    return RenderResult<void>{cxx::unexpected(RenderError{kRuntimeIdentity})};
                }
                if (binding && binding->error.type != 0)
                {
                    return RenderResult<void>{cxx::unexpected(binding->error)};
                }
                if (binding && !binding->attachment.isNull() && binding->adopted.revision == desired->revision)
                {
                    return RenderResult<void>{};
                }
            }
            return {};
        }

        static void deliverCameraTarget(CameraTargetWaiter& waiter, RenderResult<void> result) noexcept
        {
            auto code_lifetime = std::move(waiter.code_lifetime);
            waiter.complete(waiter, std::move(result));
        }

        void awaitCameraTarget(CameraTargetWaiter& waiter) noexcept
        {
            if (auto ready = cameraTargetCompletion(waiter))
            {
                deliverCameraTarget(waiter, std::move(*ready));
                return;
            }
            waiter.next = std::exchange(camera_target_waiters, &waiter);
        }

        void completeCameraTargets() noexcept
        {
            auto** link = &camera_target_waiters;
            while (*link)
            {
                auto* waiter = *link;
                auto result = cameraTargetCompletion(*waiter);
                if (!result)
                {
                    link = &waiter->next;
                    continue;
                }
                *link = waiter->next;
                deliverCameraTarget(*waiter, std::move(*result));
            }
        }

        const CameraTarget* cameraTarget(Entity camera, CameraTargetId id) const noexcept
        {
            const auto* outputs = registry.valid(camera) ? registry.try_get<CameraTargetsComponent>(camera) : nullptr;
            if (!outputs)
            {
                return nullptr;
            }
            const auto found = std::ranges::find(outputs->targets, id, &CameraTarget::id);
            return found == outputs->targets.end() ? nullptr : &*found;
        }

        CameraRenderTarget* cameraBinding(Entity camera, CameraTargetId id) const noexcept
        {
            auto* views = registry.valid(camera) ? registry.try_get<CameraRenderResourceComponent>(camera) : nullptr;
            if (!views)
            {
                return nullptr;
            }
            const auto found = std::ranges::find(views->bindings, id, &CameraRenderTarget::id);
            return found == views->bindings.end() ? nullptr : &*found;
        }

        RenderResult<CameraInput> cameraParameters(Entity camera, const CameraTarget& target) const noexcept
        {
            const bool available = registry.valid(camera) && registry.all_of<Camera, WorldTransform3D>(camera);
            if (!available)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            if (!camera_input_enabled && !compose_inputs)
            {
                return CameraInput{};
            }
            const auto& definition = registry.get<Camera>(camera);
            auto prepared = detail::prepareSceneCamera(definition, registry.get<WorldTransform3D>(camera).value);
            return prepared ? detail::sceneParameters(definition, *prepared, target)
                            : cxx::unexpected(prepared.error());
        }

        template <class F> void forEachBinding(F&& function) const
        {
            for (const auto camera : registry.view<CameraRenderResourceComponent>())
            {
                for (auto& binding : registry.get<CameraRenderResourceComponent>(camera).bindings)
                {
                    function(camera, binding);
                }
            }
        }

        bool hasViews() const noexcept
        {
            return !registry.view<CameraRenderResourceComponent>().empty() || !retiring_views.empty();
        }

        void reconcileCameraTargets() noexcept
        {
            for (const auto camera : view_changes)
            {
                if (!registry.valid(camera))
                {
                    continue;
                }
                const auto* desired = registry.try_get<CameraTargetsComponent>(camera);
                const bool available =
                    desired && !desired->targets.empty() && registry.all_of<Camera, WorldTransform3D>(camera);
                if (!available)
                {
                    registry.remove<CameraRenderResourceComponent>(camera);
                    continue;
                }
                auto& bindings = registry.get_or_emplace<CameraRenderResourceComponent>(camera).bindings;
                for (auto entry = bindings.begin(); entry != bindings.end();)
                {
                    if (std::ranges::find(desired->targets, entry->id, &CameraTarget::id) != desired->targets.end())
                    {
                        ++entry;
                        continue;
                    }
                    entry->adopted.active = false;
                    entry->output_changed = true;
                    retiring_views.push_back({camera, std::move(*entry)});
                    entry = bindings.erase(entry);
                }
                for (const auto& target : desired->targets)
                {
                    if (std::ranges::find(bindings, target.id, &CameraRenderTarget::id) == bindings.end())
                    {
                        bindings.push_back(CameraRenderTarget{.id = target.id});
                    }
                }
            }
        }

        exec::task<RenderResult<void>> compile(Change& change) noexcept
        {
            if (change.view.isNull())
            {
                CreateViewRequest request{scene};
                const auto response = co_await this->request<CreateView>(request);
                if (response.error.type != 0)
                {
                    co_return cxx::unexpected(response.error);
                }
                change.view = response.view;
                change.created_view = true;
            }
            CreateEffectRequest request{
                change.target.description,
                implementation,
                configuration.configuration,
                std::move(change.inputs)
            };
            const auto compiled = co_await this->request<CreateEffect>(request);
            if (compiled.error.type != 0)
            {
                co_return cxx::unexpected(compiled.error);
            }
            change.effect = compiled.effect;
            const auto bytes = std::as_bytes(std::span{&change.parameters, 1});
            AttachmentRequest attach{
                scene,
                change.view,
                change.target.target,
                change.effect,
                camera_input_enabled ? std::vector<std::byte>{bytes.begin(), bytes.end()} : std::vector<std::byte>{},
                change.target.viewport,
                {},
                change.replaced
            };
            attach.parameter_type = camera_input_enabled ? cxx::typeToken<CameraInput>() : cxx::TypeToken{};
            const auto adopted = co_await this->request<Attach>(attach);
            if (adopted.error.type != 0)
            {
                co_return cxx::unexpected(adopted.error);
            }
            change.attachment = adopted.attachment;
            co_return RenderResult<void>{};
        }

        void start(Change& change) noexcept
        {
            change.pending = true;
            auto work = execution::taskResult(stdexec::starts_on(execution.mainScheduler(), compile(change)));
            if (!execution.spawn(stdexec::then(
                    std::move(work),
                    [this, &change](auto result) noexcept
                    {
                        if (!result)
                        {
                            const auto* domain = result.error().domainFailure();
                            change.error = domain ? *domain : RenderError{kRuntimeState};
                        }
                        change.pending = false; // Result storage only; maintenance adopts it between Scene steps.
                    }
                )))
            {
                std::terminate();
            }
        }

        void adoptChanges() noexcept
        {
            for (auto entry = changes.begin(); entry != changes.end();)
            {
                auto& change = *entry;
                if (change.pending)
                {
                    ++entry;
                    continue;
                }
                auto* binding = cameraBinding(change.camera, change.target.id);
                const auto* target = cameraTarget(change.camera, change.target.id);
                auto parameters = cameraParameters(change.camera, change.target);
                const bool available = !closing && binding && target && parameters;
                const bool current =
                    available && target->revision == change.target.revision &&
                    input_revision == change.input_revision &&
                    cameraGraphInputs(registry.get<Camera>(change.camera), *target, compose_inputs != nullptr) ==
                        change.graph_inputs;
                if (change.error.type == 0 && current)
                {
                    if (!binding->effect.isNull())
                    {
                        retiring_views.push_back(
                            {change.camera, CameraRenderTarget{.id = binding->id, .effect = binding->effect}}
                        );
                    }
                    binding->view = change.view;
                    binding->effect = change.effect;
                    binding->attachment = change.attachment;
                    binding->adopted = *target;
                    binding->graph_inputs = change.graph_inputs;
                    binding->parameters = *parameters;
                    binding->input_revision = change.input_revision;
                    binding->effect_revision = change.effect_revision;
                    binding->parameters_changed = true;
                    binding->output_changed = true;
                    binding->error = {};
                    registry.patch<CameraRenderResourceComponent>(change.camera);
                }
                else
                {
                    if (!change.attachment.isNull() && binding && binding->attachment == change.replaced)
                    {
                        // Attach replaced the relation on RenderThread. Stop publishing the revoked ID;
                        // the stale candidate is withdrawn before the next recipe for this target.
                        binding->attachment = {};
                        binding->parameters_changed = true;
                    }
                    if (!change.effect.isNull() || change.created_view)
                    {
                        retiring_views.push_back(
                            {change.camera,
                             CameraRenderTarget{
                                 .id = change.target.id,
                                 .view = change.created_view ? change.view : ViewId{},
                                 .effect = change.effect,
                                 .attachment = change.attachment
                             }}
                        );
                    }
                    if (binding && change.error.type != 0)
                    {
                        binding->error = change.error;
                        reportRenderFailure(change.error, "Compile Camera output candidate");
                    }
                    const bool retry = change.error.type == 0 || change.error.type == kDeviceBusy ||
                                       change.error.type == kRuntimeCapacity ||
                                       change.error.type == kFrameResynchronization;
                    if (retry && !closing)
                    {
                        view_changes.insert(change.camera);
                    }
                }
                entry = changes.erase(entry);
            }
        }

        template <RenderCommand C> exec::task<RenderResult<void>> remove(typename C::Request value) noexcept
        {
            for (;;)
            {
                if (client.serverStopped())
                {
                    co_return RenderResult<void>{};
                }
                const auto result = co_await request<C>(value);
                const bool retry =
                    result.error.type == kCommandCancelled || result.error.type == kFrameResynchronization;
                if (!retry)
                {
                    if (result.error.type != 0)
                    {
                        co_return cxx::unexpected(result.error);
                    }
                    co_return RenderResult<void>{};
                }
                co_await client.whenSubmittedFramesComplete();
                co_await stdexec::schedule(execution.mainScheduler());
            }
        }

        exec::task<RenderResult<void>> withdraw() noexcept
        {
            auto& binding = exit_work->binding;
            if (!binding.adopted.target.isNull() && !binding.adopted.active)
            {
                SetTargetActive::Request deactivate{binding.adopted.target, false, binding.adopted.epoch};
                auto result = co_await remove<SetTargetActive>(deactivate);
                if (!result)
                {
                    co_return result;
                }
            }
            if (!binding.view.isNull())
            {
                auto removed = co_await remove<RemoveView>(ViewRequest{scene, binding.view});
                if (!removed)
                {
                    co_return removed;
                }
                binding.view = {};
                binding.attachment = {};
            }
            else if (!binding.attachment.isNull())
            {
                auto detached = co_await remove<Detach>(binding.attachment);
                if (!detached)
                {
                    co_return detached;
                }
                binding.attachment = {};
            }
            if (!binding.effect.isNull())
            {
                auto removed = co_await remove<RemoveEffect>(EffectRequest{binding.effect});
                if (!removed)
                {
                    co_return removed;
                }
                binding.effect = {};
            }
            co_return RenderResult<void>{};
        }

        bool changing(Entity entity, CameraTargetId id) const noexcept
        {
            return std::ranges::any_of(
                changes,
                [&](const auto& change) noexcept { return change.camera == entity && change.target.id == id; }
            );
        }

        bool withdrawing(Entity entity, CameraTargetId id) const noexcept
        {
            return (exit_work && exit_work->camera == entity && exit_work->binding.id == id) ||
                   std::ranges::any_of(
                       retiring_views,
                       [&](const auto& retired) noexcept
                       {
                           return retired.camera == entity && retired.binding.id == id &&
                                  (!retired.binding.view.isNull() || !retired.binding.attachment.isNull());
                       }
                   );
        }

        void retireViews() noexcept
        {
            if (exit_work)
            {
                if (exit_pending)
                {
                    return;
                }
                if (exit_error.type != 0)
                {
                    error = exit_error;
                    return;
                }
                exit_work.reset();
            }
            auto selected = retiring_views.end();
            for (auto entry = retiring_views.begin(); entry != retiring_views.end(); ++entry)
            {
                if (changing(entry->camera, entry->binding.id))
                {
                    continue;
                }
                if (selected == retiring_views.end() || entry->binding.view.isNull())
                {
                    selected = entry;
                }
                if (entry->binding.view.isNull())
                {
                    break;
                }
            }
            if (selected == retiring_views.end())
            {
                return;
            }
            exit_work.emplace(std::move(*selected));
            retiring_views.erase(selected);
            exit_pending = true;
            auto work = execution::taskResult(stdexec::starts_on(execution.mainScheduler(), withdraw()));
            if (!execution.spawn(stdexec::then(
                    std::move(work),
                    [this](auto result) noexcept
                    {
                        if (!result)
                        {
                            const auto* domain = result.error().domainFailure();
                            exit_error = domain ? *domain : RenderError{kRuntimeState};
                        }
                        exit_pending = false;
                    }
                )))
            {
                std::terminate();
            }
        }

        void prepareChanges() noexcept
        {
            const auto graph_revision = recipe_revision ? recipe_revision(input_object) : 0;
            if (observed_inputs != input_revision)
            {
                for (const auto camera : registry.view<CameraRenderResourceComponent>())
                {
                    view_changes.insert(camera);
                }
                observed_inputs = input_revision;
            }
            reconcileCameraTargets();
            auto dirty = std::exchange(view_changes, {});
            for (const auto camera : dirty)
            {
                auto* outputs =
                    registry.valid(camera) ? registry.try_get<CameraRenderResourceComponent>(camera) : nullptr;
                if (!outputs)
                {
                    continue;
                }
                for (auto& binding : outputs->bindings)
                {
                    if (changing(camera, binding.id))
                    {
                        continue;
                    }
                    if (withdrawing(camera, binding.id))
                    {
                        view_changes.insert(camera);
                        continue;
                    }
                    const auto* target = cameraTarget(camera, binding.id);
                    if (!target)
                    {
                        continue;
                    }
                    auto parameters = cameraParameters(camera, *target);
                    if (!parameters)
                    {
                        binding.error = parameters.error();
                        binding.adopted.active = false;
                        binding.output_changed = true;
                        continue;
                    }
                    const auto key =
                        cameraGraphInputs(registry.get<Camera>(camera), *target, compose_inputs != nullptr);
                    if (!binding.attachment.isNull() && binding.graph_inputs == key &&
                        binding.effect_revision == graph_revision)
                    {
                        binding.parameters_changed |= !sameParameters(binding.parameters, *parameters) ||
                                                      binding.adopted.viewport != target->viewport;
                        binding.output_changed |= binding.adopted.description != target->description ||
                                                  binding.adopted.active != target->active ||
                                                  binding.adopted.epoch != target->epoch;
                        binding.parameters = *parameters;
                        binding.adopted = *target;
                        binding.input_revision = input_revision;
                        binding.error = {};
                        continue;
                    }
                    auto data = compose_inputs ? compose_inputs(
                                                     input_object,
                                                     cxx::typeToken<CameraInput>(),
                                                     std::as_bytes(std::span{&*parameters, 1})
                                                 )
                                               : RenderResult<ConfigValue>{ConfigValue{}};
                    if (!data)
                    {
                        binding.error = data.error();
                        continue;
                    }
                    auto& change = changes.emplace_back();
                    change.camera = camera;
                    change.target = *target;
                    change.graph_inputs = key;
                    change.parameters = *parameters;
                    change.inputs = std::move(*data);
                    change.view = binding.view;
                    change.replaced = binding.attachment;
                    change.input_revision = input_revision;
                    change.effect_revision = graph_revision;
                    start(change);
                }
            }
        }

        void confirmInputs() noexcept
        {
            const bool withdrawing_inputs =
                (exit_work && (!exit_work->binding.view.isNull() || !exit_work->binding.attachment.isNull())) ||
                std::ranges::any_of(
                    retiring_views,
                    [](const auto& value) noexcept
                    { return !value.binding.view.isNull() || !value.binding.attachment.isNull(); }
                );
            if (!changes.empty() || withdrawing_inputs)
            {
                return;
            }
            bool adopted = true;
            forEachBinding([&](Entity, const CameraRenderTarget& value) noexcept
                           { adopted &= value.attachment.isNull() || value.input_revision == input_revision; });
            if (adopted)
            {
                adopted_revision = input_revision;
            }
        }
    };

    CameraViewBinding::CameraViewBinding(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    RenderResult<std::unique_ptr<CameraViewBinding>> CameraViewBinding::create(RenderBindingContext& context) noexcept
    {
        auto view = context.client.bind<CreateView>();
        auto effect = context.client.bind<CreateEffect>();
        auto attach = context.client.bind<Attach>();
        auto detach = context.client.bind<Detach>();
        auto remove_view = context.client.bind<RemoveView>();
        auto remove_effect = context.client.bind<RemoveEffect>();
        auto active = context.client.bind<SetTargetActive>();
        const bool missing = !view || !effect || !attach || !detach || !remove_view || !remove_effect || !active;
        if (missing)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        auto impl = std::make_unique<Impl>(
            context,
            Impl::Commands{*view, *effect, *attach, *detach, *remove_view, *remove_effect, *active}
        );
        return std::unique_ptr<CameraViewBinding>{new CameraViewBinding(std::move(impl))};
    }

    CameraViewBinding::~CameraViewBinding() noexcept
    {
        if (!impl_->changes.empty() || impl_->exit_work || impl_->hasViews() || impl_->camera_target_waiters)
        {
            std::terminate();
        }
    }

    void CameraViewBinding::cameraChanged(Entity camera) noexcept
    {
        impl_->view_changes.insert(camera);
    }

    void CameraViewBinding::bindingsDestroyed(Entity camera) noexcept
    {
        auto& self = *impl_;
        for (auto& binding : self.registry.get<CameraRenderResourceComponent>(camera).bindings)
        {
            binding.adopted.active = false;
            binding.output_changed = true;
            self.retiring_views.push_back({camera, std::move(binding)});
        }
    }

    SceneStageResult CameraViewBinding::maintain(SceneStageContext& context) noexcept
    {
        auto& self = *impl_;
        self.closing |= context.stop.stop_requested();
        if (self.closing && self.client.serverStopped())
        {
            self.error = {};
            if (!self.exit_pending)
            {
                self.exit_error = {};
            }
        }
        context.publication_needed |= !self.view_changes.empty() || !self.changes.empty() ||
                                      self.exit_work.has_value() || self.observed_inputs != self.input_revision;
        if (!context.allow_structure && !self.closing)
        {
            return ESceneProgress::COMPLETE;
        }
        self.adoptChanges();
        if (self.closing)
        {
            self.registry.clear<CameraRenderResourceComponent>();
            self.view_changes.clear();
        }
        else
        {
            self.prepareChanges();
        }
        self.retireViews();
        self.confirmInputs();
        self.completeCameraTargets();
        if (self.error.type != 0)
        {
            return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, self.error});
        }
        const bool pending = !self.changes.empty() || self.exit_work || !self.retiring_views.empty();
        return self.closing && pending ? ESceneProgress::PENDING : ESceneProgress::COMPLETE;
    }

    RenderResult<CameraTargetId> CameraViewBinding::addCameraTarget(Entity camera, CameraTarget target) noexcept
    {
        auto& self = *impl_;
        const bool available = self.registry.valid(camera) && self.registry.all_of<Camera>(camera);
        if (!available || self.closing || target.target.isNull() || !target.viewport.validate())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        if (self.next_camera_target == UINT64_MAX)
        {
            std::terminate(); // Scene-local identities cannot wrap or be reused.
        }
        target.id = CameraTargetId{++self.next_camera_target};
        target.revision = 1;
        static_cast<void>(self.registry.get_or_emplace<CameraTargetsComponent>(camera));
        self.registry.patch<CameraTargetsComponent>(
            camera,
            [&](auto& component) { component.targets.push_back(target); }
        );
        return target.id;
    }

    bool CameraViewBinding::updateCameraTarget(Entity camera, CameraTargetId id, CameraTarget target) noexcept
    {
        auto& self = *impl_;
        const auto* current = self.cameraTarget(camera, id);
        if (!current || self.closing || target.target.isNull() || !target.viewport.validate())
        {
            return false;
        }
        target.id = id;
        target.revision = current->revision + 1;
        self.registry.patch<CameraTargetsComponent>(
            camera,
            [&](auto& component) { *std::ranges::find(component.targets, id, &CameraTarget::id) = target; }
        );
        return true;
    }

    bool CameraViewBinding::removeCameraTarget(Entity camera, CameraTargetId id) noexcept
    {
        auto& self = *impl_;
        if (!self.cameraTarget(camera, id))
        {
            return false;
        }
        self.registry.patch<CameraTargetsComponent>(
            camera,
            [&](auto& component)
            { std::erase_if(component.targets, [&](const auto& value) noexcept { return value.id == id; }); }
        );
        return true;
    }

    AttachmentId CameraViewBinding::attachment(Entity camera, CameraTargetId id) const noexcept
    {
        const auto* binding = impl_->cameraBinding(camera, id);
        return binding ? binding->attachment : AttachmentId{};
    }

    RenderError CameraViewBinding::cameraTargetError(Entity camera, CameraTargetId id) const noexcept
    {
        const auto* binding = impl_->cameraBinding(camera, id);
        if (binding)
        {
            return binding->error;
        }
        if (!impl_->cameraTarget(camera, id))
        {
            return {};
        }
        // A newly edited Transform can still be awaiting the existing TransformSystem's stable point.
        const bool has_pose = impl_->registry.any_of<Transform3D, WorldTransform3D>(camera);
        const bool missing_camera = !impl_->registry.all_of<Camera>(camera) || !has_pose;
        return missing_camera ? RenderError{kCommandContract} : RenderError{};
    }

    bool CameraViewBinding::cameraTargetRetired(Entity camera, CameraTargetId id) const noexcept
    {
        return impl_->cameraTargetRetired(camera, id);
    }

    exec::task<RenderResult<void>> CameraViewBinding::whenCameraTargetReady(Entity camera, CameraTargetId id) noexcept
    {
        co_return co_await stdexec::continues_on(
            Impl::CameraTargetSender{*impl_, camera, id, false},
            impl_->execution.mainScheduler()
        );
    }

    exec::task<RenderResult<void>> CameraViewBinding::whenCameraTargetRetired(Entity camera, CameraTargetId id) noexcept
    {
        co_return co_await stdexec::continues_on(
            Impl::CameraTargetSender{*impl_, camera, id, true},
            impl_->execution.mainScheduler()
        );
    }

} // namespace lux::scene
