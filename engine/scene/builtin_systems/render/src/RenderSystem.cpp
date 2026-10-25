#include <list>
#include <lux/engine/context/render/RenderAssets.hpp>
#include <lux/engine/context/render/RenderDiagnostics.hpp>
#include <lux/engine/scene/RenderBindingInstance.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfigurationImpl.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/render/RenderBindingPlan.hpp>

namespace lux::scene
{
    using namespace lux::render;
    using namespace engine::render;
    using namespace simulation::ecs;

    struct RenderSystem::Impl final
    {
        Registry& registry;
        RenderRequester& client;
        execution::ExecutionRuntime& execution;
        RenderAssets& assets;
        RenderAssetInput& asset_input;
        process::asset_loading::AssetReadPort source;
        RenderBindingPlan plan;
        std::vector<detail::RenderBindingInstance> bindings;

        struct ViewBindings
        {
            ViewId view;
            Entity entity;
            std::vector<detail::RenderBindingInstance> bindings;
            bool retiring = false;

            ~ViewBindings() noexcept
            {
                while (!bindings.empty())
                {
                    bindings.pop_back();
                }
            }
        };

        std::list<ViewBindings> views;
        SceneId scene;
        std::uint64_t input_revision = 1, adopted_revision = 0;
        RenderError error;
        bool starting = false, installed = false, ready = false, closing = false, pending = false;

        struct ReadyWaiter
        {
            ReadyWaiter* next = nullptr;
            void (*complete)(ReadyWaiter&, RenderResult<void>) noexcept {};
            std::shared_ptr<const void> code_lifetime;
        };

        ReadyWaiter* waiters = nullptr;

        struct ReadySender
        {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(RenderResult<void>)>;
            Impl& system;

            template <class Receiver> struct Operation : ReadyWaiter
            {
                using operation_state_concept = stdexec::operation_state_t;
                Impl& system;
                Receiver receiver;

                Operation(Impl& system, Receiver receiver) noexcept : system(system), receiver(std::move(receiver))
                {
                    this->code_lifetime = execution::detail::getCompletionCode(stdexec::get_env(this->receiver));
                    this->complete = [](ReadyWaiter& waiter, RenderResult<void> result) noexcept
                    {
                        auto& operation = static_cast<Operation&>(waiter);
                        stdexec::set_value(std::move(operation.receiver), std::move(result));
                    };
                }

                void start() & noexcept
                {
                    system.awaitReady(*this);
                }
            };

            template <class Receiver> auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{system, std::move(receiver)};
            }
        };

        Impl(Registry& registry, const RenderSystemDependencies& dependencies)
            : registry(registry), client(dependencies.client), execution(dependencies.execution),
              assets(dependencies.assets), asset_input(dependencies.assets_input), source(dependencies.source),
              plan(dependencies.plan)
        {
        }

        ~Impl() noexcept
        {
            // Scene leases end before destruction; dependent destructors still need their providers.
            while (!bindings.empty())
            {
                bindings.pop_back();
            }
        }

        static void* findIn(
            std::span<const detail::RenderBindingInstance> entries,
            cxx::TypeToken type,
            std::string_view role
        ) noexcept
        {
            void* found = nullptr;
            for (const auto& binding : entries)
            {
                if (binding.declaration.type != type || (!role.empty() && binding.declaration.role != role))
                {
                    continue;
                }
                if (found)
                {
                    return nullptr;
                }
                found = binding.object.get();
            }
            return found;
        }

        void* find(cxx::TypeToken type, std::string_view role, ViewId view = {}) noexcept
        {
            if (!view.isNull())
            {
                auto found = std::ranges::find(views, view, &ViewBindings::view);
                if (found != views.end())
                {
                    if (auto* object = findIn(found->bindings, type, role))
                    {
                        return object;
                    }
                }
            }
            return findIn(bindings, type, role);
        }

        void visit(cxx::TypeToken type, ViewId view, void* receiver, void (*apply)(void*, void*) noexcept) noexcept
        {
            for (auto& binding : bindings)
            {
                if (binding.declaration.type == type)
                {
                    apply(receiver, binding.object.get());
                }
            }
            auto found = std::ranges::find(views, view, &ViewBindings::view);
            if (found != views.end())
            {
                for (auto& binding : found->bindings)
                {
                    if (binding.declaration.type == type)
                    {
                        apply(receiver, binding.object.get());
                    }
                }
            }
        }

        static void deliver(ReadyWaiter& waiter, RenderResult<void> result) noexcept
        {
            auto code = std::move(waiter.code_lifetime);
            waiter.complete(waiter, std::move(result));
        }

        RenderResult<void> readiness() const noexcept
        {
            if (error.type != 0)
            {
                return cxx::unexpected(error);
            }
            if (closing)
            {
                return cxx::unexpected(RenderError{kCommandCancelled});
            }
            return {};
        }

        void awaitReady(ReadyWaiter& waiter) noexcept
        {
            if (ready || error.type != 0 || closing)
            {
                deliver(waiter, readiness());
                return;
            }
            waiter.next = std::exchange(waiters, &waiter);
        }

        void completeReady() noexcept
        {
            if (!ready && error.type == 0 && !closing)
            {
                return;
            }
            while (waiters)
            {
                auto* waiter = waiters;
                waiters = waiter->next;
                deliver(*waiter, readiness());
            }
        }

        exec::task<RenderResult<void>> initialize() noexcept
        {
            for (const auto& declaration : plan.declarations)
            {
                if (declaration.prepare_resources)
                {
                    auto prepared = co_await declaration.prepare_resources(assets);
                    if (!prepared)
                    {
                        co_return prepared;
                    }
                }
            }
            auto create = client.bind<CreateScene>();
            auto install = client.bind<InstallCapabilities>();
            if (!create || !install)
            {
                co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            EmptyRequest empty;
            const auto created = co_await client.request(*create, empty);
            if (created.error.type != 0)
            {
                co_return cxx::unexpected(created.error);
            }
            scene = created.scene;
            CapabilityInstallation request{plan.implementation, scene, plan.configuration.configuration};
            const auto inputs = assets.inputs();
            request.source_inputs.assign(inputs.begin(), inputs.end());
            auto result = co_await client.request(*install, request);
            if (result.error.type != 0)
            {
                co_return cxx::unexpected(result.error);
            }
            auto resolved = resolveRenderBindingInputs(plan, result.inputs, assets.inputs());
            if (!resolved)
            {
                co_return resolved;
            }
            installed = true;
            co_return RenderResult<void>{};
        }

        RenderBindingContext bindingContext(
            const RenderBindingDeclaration& declaration,
            ViewId view,
            Entity entity
        ) noexcept
        {
            RenderBindingContext context{
                registry,
                client,
                execution,
                declaration.destinations,
                plan.contracts,
                assets,
                asset_input,
                source,
                scene,
                plan.implementation,
                plan.configuration,
                input_revision,
                adopted_revision,
                this,
                [](void* data, cxx::TypeToken type, std::string_view role, ViewId view) noexcept
                { return static_cast<Impl*>(data)->find(type, role, view); },
                [](void* data, cxx::TypeToken type, ViewId view, void* receiver, void (*apply)(void*, void*) noexcept
                ) noexcept { static_cast<Impl*>(data)->visit(type, view, receiver, apply); },
                [](void* data, ViewId view, Entity entity) noexcept
                { return static_cast<Impl*>(data)->bindView(view, entity); },
                [](void* data, ViewId view, SceneStageContext& stage) noexcept
                { return static_cast<Impl*>(data)->finishView(view, stage); }
            };
            context.view = view;
            context.view_entity = entity;
            for (auto& binding : bindings)
            {
                if (binding.declaration.effect_inputs)
                {
                    context.effect_input_object = binding.object.get();
                    context.effect_inputs = binding.declaration.effect_inputs;
                    context.effect_revision = binding.declaration.effect_revision;
                }
            }
            return context;
        }

        RenderResult<void> constructBindings(
            std::vector<detail::RenderBindingInstance>& output,
            ERenderOperationScope scope,
            ViewId view = {},
            Entity entity = NullEntity
        ) noexcept
        {
            output.reserve(std::ranges::count(plan.declarations, scope, &RenderBindingDeclaration::scope));
            for (const auto& declaration : plan.declarations)
            {
                if (declaration.scope != scope)
                {
                    continue;
                }
                auto context = bindingContext(declaration, view, entity);
                context.role = declaration.role;
                context.role_index = declaration.role_index;
                auto created = declaration.create(context);
                if (!created)
                {
                    return cxx::unexpected(created.error());
                }
                auto& instance = output.emplace_back(declaration, *created);
                declaration.connect(instance.object, registry, instance.connections);
            }
            return {};
        }

        RenderResult<void> connect() noexcept
        {
            auto connected = constructBindings(bindings, ERenderOperationScope::SCENE);
            ready = bool(connected);
            return connected;
        }

        RenderResult<void> bindView(ViewId view, Entity entity) noexcept
        {
            if (closing || view.isNull())
            {
                return cxx::unexpected(RenderError{kRuntimeState});
            }
            auto found = std::ranges::find(views, view, &ViewBindings::view);
            if (found != views.end())
            {
                return found->entity == entity && !found->retiring ? RenderResult<void>{}
                                                                   : cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            auto& entry = views.emplace_back(view, entity);
            auto result = constructBindings(entry.bindings, ERenderOperationScope::VIEW, view, entity);
            if (!result)
            {
                // Constructors only capture dependencies; no accepted work precedes stage execution.
                views.pop_back();
            }
            return result;
        }

        static SceneStageContext stoppedStage(const SceneStageContext& stage) noexcept
        {
            // One immutable requested token adapts local View retirement to the existing stage contract.
            // There is no per-View cancellation source, Scope, operation owner or completion queue.
            static const auto requested = []
            {
                std::stop_source stop;
                stop.request_stop();
                return stop;
            }();
            auto result = stage;
            result.stop = requested.get_token();
            return result;
        }

        SceneStageResult finishView(ViewId view, SceneStageContext& stage) noexcept
        {
            const auto found = std::ranges::find(views, view, &ViewBindings::view);
            if (found == views.end())
            {
                return ESceneProgress::COMPLETE;
            }
            found->retiring = true;
            auto stopped = stoppedStage(stage);
            for (auto entry = found->bindings.rbegin(); entry != found->bindings.rend(); ++entry)
            {
                if (!entry->declaration.maintain)
                {
                    continue;
                }
                auto result = entry->declaration.maintain(entry->object.get(), stopped);
                stage.publication_needed |= stopped.publication_needed;
                if (!result || *result == ESceneProgress::PENDING)
                {
                    return result;
                }
            }
            // Only after all accepted View work is settled can the provider remove its native View.
            views.erase(found);
            return ESceneProgress::COMPLETE;
        }

        static SceneStageResult publishBinding(
            detail::RenderBindingInstance& binding,
            SceneStageContext& context,
            engine::render::FrameInput& frame
        ) noexcept
        {
            auto result = binding.declaration.publish(binding.object.get(), context, frame);
            // Different roles may write the same destination. Only the behavior can assert that
            // commands within its group are independent; never infer independence between behaviors.
            if (frame.group_ends.empty() || frame.group_ends.back() != frame.commands.size())
            {
                frame.group_ends.push_back(frame.commands.size());
            }
            return result;
        }

        exec::task<RenderResult<void>> removeScene() noexcept
        {
            if (!client.serverStopped())
            {
                auto command = client.bind<RemoveScene>();
                if (!command)
                {
                    co_return cxx::unexpected(command.error());
                }
                SceneRequest input{scene};
                auto result = co_await client.request(*command, input);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
            }
            scene = {};
            co_return RenderResult<void>{};
        }

        void start(exec::task<RenderResult<void>> work) noexcept
        {
            pending = true;
            auto sender = execution::taskResult(stdexec::starts_on(execution.mainScheduler(), std::move(work)));
            if (!execution.spawn(stdexec::then(
                    std::move(sender),
                    [this](auto result) noexcept
                    {
                        if (!result)
                        {
                            const auto* domain = result.error().domainFailure();
                            error = domain ? *domain : RenderError{kRuntimeState};
                            reportRenderFailure(error, closing ? "Remove Render Scene" : "Install Scene render inputs");
                        }
                        pending = false;
                        completeReady();
                    }
                )))
            {
                std::terminate();
            }
        }
    };

    RenderSystem::RenderSystem(Registry& registry, const RenderSystemDependencies& dependencies)
        : impl_(std::make_unique<Impl>(registry, dependencies))
    {
    }

    RenderSystem::~RenderSystem() noexcept
    {
        if (impl_->pending || impl_->waiters || !impl_->scene.isNull() || !impl_->views.empty())
        {
            std::terminate();
        }
    }

    void* RenderSystem::findBinding(cxx::TypeToken type, std::string_view role) noexcept
    {
        return impl_->find(type, role);
    }

    exec::task<RenderResult<void>> RenderSystem::whenReady() noexcept
    {
        co_return co_await stdexec::continues_on(Impl::ReadySender{*impl_}, impl_->execution.mainScheduler());
    }

    SceneStageResult RenderSystem::maintain(SceneStageContext& context) noexcept
    {
        auto& self = *impl_;
        self.closing |= context.stop.stop_requested();
        if (self.closing)
        {
            self.completeReady();
            if (self.pending)
            {
                return ESceneProgress::PENDING;
            }
            while (!self.views.empty())
            {
                auto result = self.finishView(self.views.front().view, context);
                if (!result || *result == ESceneProgress::PENDING)
                {
                    return result;
                }
            }
            // Reverse the actual construction dependencies. Camera outputs finish before their
            // input producers release Scene/global resources. No second cleanup Scope is created.
            for (auto it = self.bindings.rbegin(); it != self.bindings.rend(); ++it)
            {
                if (!it->declaration.maintain)
                {
                    continue;
                }
                auto result = it->declaration.maintain(it->object.get(), context);
                if (!result || *result == ESceneProgress::PENDING)
                {
                    return result;
                }
            }
            for (auto& binding : self.bindings)
            {
                binding.connections.clear();
            }
            // Quiescent behavior objects remain address-stable for the Scene lease. A failed Scene
            // can still have callers awaiting output retirement; its destructor ends those borrows.
            if (!self.scene.isNull())
            {
                self.error = {};
                self.start(self.removeScene());
                return ESceneProgress::PENDING;
            }
            return ESceneProgress::COMPLETE;
        }
        if (self.client.serverStopped())
        {
            // No future frame can finish a pending publication. Report the terminal transport fact
            // so SceneDriver rejects queued edits and enters its existing stop/cleanup path.
            return cxx::unexpected(
                SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, RenderError{kCommandCancelled}}
            );
        }
        if (self.error.type != 0)
        {
            return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, self.error});
        }
        if (!self.ready)
        {
            if (!self.starting)
            {
                self.starting = true;
                self.start(self.initialize());
            }
            if (!self.pending && self.installed && context.allow_structure)
            {
                auto connected = self.connect();
                if (!connected)
                {
                    self.error = connected.error();
                }
                self.completeReady();
            }
            if (!self.ready)
            {
                return ESceneProgress::PENDING;
            }
        }
        bool pending = false;
        for (auto& binding : self.bindings)
        {
            context.publication_needed |= binding.declaration.each_frame;
            if (!binding.declaration.maintain)
            {
                continue;
            }
            auto result = binding.declaration.maintain(binding.object.get(), context);
            if (!result)
            {
                return result;
            }
            pending |= *result == ESceneProgress::PENDING;
        }
        for (auto& view : self.views)
        {
            if (view.retiring)
            {
                continue;
            }
            for (auto& binding : view.bindings)
            {
                context.publication_needed |= binding.declaration.each_frame;
                if (!binding.declaration.maintain)
                {
                    continue;
                }
                auto result = binding.declaration.maintain(binding.object.get(), context);
                if (!result)
                {
                    return result;
                }
                pending |= *result == ESceneProgress::PENDING;
            }
        }
        const bool has_outputs =
            std::ranges::any_of(self.plan.declarations, [](const auto& value) noexcept { return value.view_provider; });
        if (!has_outputs)
        {
            self.adopted_revision = self.input_revision;
        }
        return pending ? ESceneProgress::PENDING : ESceneProgress::COMPLETE;
    }

    SceneStageResult RenderSystem::prepareInputs(SceneStageContext& context) noexcept
    {
        if (!impl_->ready || impl_->closing)
        {
            return ESceneProgress::COMPLETE;
        }
        for (auto& binding : impl_->bindings)
        {
            if (!binding.declaration.prepare)
            {
                continue;
            }
            auto result = binding.declaration.prepare(binding.object.get(), context);
            if (!result || *result == ESceneProgress::PENDING)
            {
                return result;
            }
        }
        for (auto& view : impl_->views)
        {
            if (view.retiring)
            {
                continue;
            }
            for (auto& binding : view.bindings)
            {
                if (!binding.declaration.prepare)
                {
                    continue;
                }
                auto result = binding.declaration.prepare(binding.object.get(), context);
                if (!result || *result == ESceneProgress::PENDING)
                {
                    return result;
                }
            }
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult RenderSystem::publish(SceneStageContext& context) noexcept
    {
        if (!impl_->ready || impl_->closing)
        {
            return ESceneProgress::COMPLETE;
        }
        auto* frame = impl_->client.buildingFrame();
        if (!frame)
        {
            impl_->client.requestFrameInput();
            return ESceneProgress::PENDING;
        }
        for (auto& binding : impl_->bindings)
        {
            if (!binding.declaration.publish)
            {
                continue;
            }
            auto result = Impl::publishBinding(binding, context, *frame);
            if (!result || *result == ESceneProgress::PENDING)
            {
                return result;
            }
        }
        for (auto& view : impl_->views)
        {
            if (view.retiring)
            {
                continue;
            }
            for (auto& binding : view.bindings)
            {
                if (!binding.declaration.publish)
                {
                    continue;
                }
                auto result = Impl::publishBinding(binding, context, *frame);
                if (!result || *result == ESceneProgress::PENDING)
                {
                    return result;
                }
            }
        }
        return ESceneProgress::COMPLETE;
    }

    SceneSystemRegistration renderSystemRegistration() noexcept
    {
        static constexpr std::array requirements{SceneSystemRequirementSpec{
            "rendering",
            "lux.scene.rendering",
            cxx::typeToken<RenderSystemDependencies>(),
            false
        }};
        return {
            .type = system::systemTypeId(RenderSystem::Description.canonical_name),
            .cpp_type = cxx::typeToken<RenderSystem>(),
            .description = &RenderSystem::Description,
            .configuration = renderConfigurationCodec(),
            .requirements = requirements,
            .install = +[](SceneSystemInstaller& builder,
                           SceneSystemDescription input) noexcept -> cxx::expected<void, SceneSystemBuildFailure>
            {
                auto* dependencies = builder.require<RenderSystemDependencies>(input.instanceId(), "rendering");
                if (!dependencies)
                {
                    return cxx::unexpected(SceneSystemBuildFailure{ESceneSystemBuildError::MISSING_REQUIREMENT});
                }
                auto configuration = builder.decodeConfiguration<RenderSystemConfiguration>(input);
                if (!configuration || !validRenderConfiguration(*configuration))
                {
                    return cxx::unexpected(
                        SceneSystemBuildFailure{ESceneSystemBuildError::INVALID_DESCRIPTION, input.instanceId()}
                    );
                }
                auto instance =
                    builder.emplaceSystem<RenderSystem>(input.instanceId(), builder.registry(), *dependencies);
                if (!instance)
                {
                    return cxx::unexpected(instance.error());
                }
                auto maintenance = builder.addMaintenanceTask<RenderSystem>(
                    input.instanceId(),
                    [](auto& system, auto& context) noexcept { return system.maintain(context); }
                );
                if (!maintenance)
                {
                    return maintenance;
                }
                auto prepared = builder.addStablePointTask<RenderSystem>(
                    input.instanceId(),
                    [](auto& system, auto& context) noexcept { return system.prepareInputs(context); }
                );
                if (!prepared)
                {
                    return prepared;
                }
                return builder.addPublicationTask<RenderSystem>(
                    input.instanceId(),
                    [](auto& system, auto& context) noexcept { return system.publish(context); }
                );
            },
            .configuration_migrations = detail::renderConfigurationMigrations(),
            .resolve_data_inputs =
                +[](SceneSystemDescription description, std::span<const SceneCapabilityProvider> providers
                 ) noexcept -> cxx::expected<std::vector<system::SystemDataContract>, SceneSystemBuildFailure>
            {
                const auto found =
                    std::ranges::find(providers, std::string_view{"rendering"}, &SceneCapabilityProvider::name);
                if (found == providers.end() || found->type != cxx::typeToken<RenderSystemDependencies>())
                {
                    return cxx::unexpected(
                        SceneSystemBuildFailure{ESceneSystemBuildError::MISSING_REQUIREMENT, description.instanceId()}
                    );
                }
                const auto& plan = static_cast<const RenderSystemDependencies*>(found->value)->plan;
                RenderSystemConfiguration configuration;
                auto decoder = description.configurationSchemaVersion() == 2
                                   ? renderConfigurationCodec().decode
                                   : detail::renderConfigurationMigrations().front().decode;
                const bool supported_version =
                    description.configurationSchemaVersion() == 1 || description.configurationSchemaVersion() == 2;
                if (!supported_version || !decoder(description.configurationPayload(), &configuration))
                {
                    return cxx::unexpected(SceneSystemBuildFailure{
                        ESceneSystemBuildError::CONFIGURATION_DECODE_FAILURE,
                        description.instanceId()
                    });
                }
                const auto& prepared = plan.configuration;
                const bool same_selection = configuration.capability == prepared.capability &&
                                            configuration.capability_version == prepared.capability_version &&
                                            configuration.implementation == prepared.implementation &&
                                            configuration.implementation_version == prepared.implementation_version;
                const bool same_configuration = configuration.configuration.schema == prepared.configuration.schema &&
                                                configuration.configuration.version == prepared.configuration.version &&
                                                configuration.configuration.bytes == prepared.configuration.bytes;
                if (!same_selection || !same_configuration || plan.implementation.isNull())
                {
                    return cxx::unexpected(
                        SceneSystemBuildFailure{ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId()}
                    );
                }
                return plan.data_inputs;
            }
        };
    }
} // namespace lux::scene
