#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <algorithm>
#include <atomic>
#include <thread>
namespace lux::editor::flowforge
{
    struct FlowCompileEnvironment::Data final
    {
        std::shared_ptr<const void> code;
        std::vector<const lux::meta::RefType*> types;
        std::vector<const lux::meta::RefClass*> classes;
        std::vector<const lux::meta::RefFunction*> functions;
        std::vector<lux::flowforge::ScriptAbilityNodeDescription> abilities;
        std::vector<lux::script::ScriptEventSourceDescription> events;
        std::uint64_t version;
    };
    FlowCompileEnvironment::FlowCompileEnvironment(lux::flowforge::FlowSourceEnvironment env, std::uint64_t version)
        : data_(std::make_shared<const Data>(Data{
              std::move(env.code_lifetime),
              {env.types.begin(), env.types.end()},
              {env.classes.begin(), env.classes.end()},
              {env.functions.begin(), env.functions.end()},
              {env.abilities.nodes().begin(), env.abilities.nodes().end()},
              {env.events.begin(), env.events.end()},
              version
          }))
    {}
    lux::flowforge::FlowSourceEnvironment FlowCompileEnvironment::view() const noexcept
    {
        return {
            data_->types,
            data_->classes,
            data_->functions,
            lux::flowforge::ScriptAbilityNodeCatalogView{data_->abilities},
            data_->events,
            data_
        };
    }
    std::uint64_t FlowCompileEnvironment::version() const noexcept
    {
        return data_->version;
    }
    namespace
    {
        std::atomic_uint64_t next_id{1};
        std::uint64_t allocateId() noexcept
        {
            auto value = next_id.load(std::memory_order_relaxed);
            while (value != UINT64_MAX)
                if (next_id.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                    return value;
            return 0;
        }
        template <class E> auto failed(E e)
        {
            return lux::cxx::unexpected(VFlowCompilationFailure{std::move(e)});
        }
        using Object = std::shared_ptr<const lux::flowforge::FlowForgeObject>;
        using ObjectResult = FlowCompilationResult<Object>;
        using CompileResult = FlowCompilationResult<std::shared_ptr<const CompiledFlow>>;
        struct Completion final
        {
            Object object;
            CompileResult result;
        };
        ObjectResult compile(
            const lux::flowforge::FlowSource& source,
            const FlowCompileEnvironment& env,
            FlowCompileSettings settings,
            std::stop_token stop
        )
        {
            if (stop.stop_requested())
                return failed(EFlowCompilationError::CANCELLED);
            auto environment = env.view();
            auto graph = lux::flowforge::materializeFlowSource(source, environment);
            if (!graph)
                return failed(graph.error());
            lux::flowforge::FlowForgeCompileOptions options;
            options.module_name = "flow_" + uuids::to_string(source.id.uuid());
            std::ranges::replace(options.module_name, '-', '_');
            options.script_abilities = environment.abilities;
            options.script_events = environment.events;
            auto result = lux::flowforge::compileFlowForgeObject(*graph, options);
            if (!result)
                return failed(result.error());
            if (result->object.size() > settings.byte_limit)
                return failed(EFlowCompilationError::CAPACITY);
            return std::make_shared<const lux::flowforge::FlowForgeObject>(std::move(*result));
        }
        template <class Prepared>
        auto finish(
            Prepared prepared,
            std::shared_ptr<const lux::flowforge::FlowSource> source,
            FlowCompileKey key,
            FlowCompileSettings settings,
            LinkSettings link,
            process::CpuScheduler cpu,
            process::BlockingScheduler blocking,
            process::TaskReporter reporter
        )
        {
            return stdexec::let_value(
                std::move(prepared),
                [source = std::move(source), key, settings, link = std::move(link), cpu, blocking, reporter](
                    ObjectResult& object
                ) {
                    auto linked = stdexec::then(
                        stdexec::schedule(blocking),
                        [&object, link, reporter]() -> FlowCompilationResult<lux::script::ScriptArtifact> {
                            if (!object)
                                return lux::cxx::unexpected(object.error());
                            if (reporter.stopToken().stop_requested())
                                return failed(EFlowCompilationError::CANCELLED);
                            reporter.setPhase("Link Flow");
                            auto artifact = lux::flowforge::linkFlowForgeObject(**object, link.executable);
                            if (!artifact)
                                return failed(artifact.error());
                            return std::move(*artifact);
                        }
                    );
                    return stdexec::then(
                        stdexec::continues_on(std::move(linked), cpu),
                        [&object, source, key, settings, reporter](
                            FlowCompilationResult<lux::script::ScriptArtifact> linked
                        ) -> FlowCompilationResult<Completion> {
                            const Object fixed = object ? *object : Object{};
                            if (!linked)
                                return Completion{fixed, lux::cxx::unexpected(linked.error())};
                            if (reporter.stopToken().stop_requested())
                                return Completion{fixed, failed(EFlowCompilationError::CANCELLED)};
                            reporter.setPhase("Encode Flow");
                            auto artifact = lux::script::ScriptArtifactAsset::create(
                                {source->id, lux::script::ScriptArtifactAsset::asset_type},
                                std::make_shared<const lux::script::ScriptArtifact>(std::move(*linked))
                            );
                            if (!artifact)
                                return Completion{fixed, failed(artifact.error())};
                            auto encoded = asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::encode(
                                **artifact,
                                asset::AssetEncodeLimits{settings.byte_limit}
                            );
                            if (!encoded)
                                return Completion{fixed, failed(encoded.error())};
                            auto bytes = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
                            return Completion{
                                fixed,
                                std::make_shared<const CompiledFlow>(CompiledFlow{
                                    key,
                                    source,
                                    std::move(*artifact),
                                    lux::cxx::SharedBytes<>::fromOwner(bytes, *bytes)
                                })
                            };
                        }
                    );
                }
            );
        }
    }
    struct FlowCompileOperation::Impl final
    {
        std::thread::id owner{std::this_thread::get_id()};
        FlowCompileId id{allocateId()};
        FlowCompileEnvironment environment;
        std::shared_ptr<const lux::flowforge::FlowSource> source;
        FlowCompileSettings settings;
        FlowCompileKey key;
        sessions::ObservationVersion observed;
        process::Task task;
        Object object;
        std::optional<CompileResult> completed;
        std::vector<FlowLinkAttempt> attempts;
        void accept(process::TTaskResult<Completion, VFlowCompilationFailure>&& result) noexcept
        {
            if (result)
            {
                object = std::move(result->object);
                completed.emplace(std::move(result->result));
            }
            else if (auto* error = result.error().domainFailure())
                completed.emplace(lux::cxx::unexpected(std::move(*error)));
            else if (auto* error = result.error().executionFailure())
                completed.emplace(failed(*error));
            else
                completed.emplace(failed(EFlowCompilationError::CANCELLED));
            attempts.back().complete = true;
            if (!*completed)
                attempts.back().failure = completed->error();
            task = {}; // Leaf completion only: never invoke a business callback or release its dispatch guard.
        }
    };
    FlowCompileOperation::FlowCompileOperation(std::shared_ptr<Impl> state) : impl_(std::move(state)) {}
    FlowCompileOperation::~FlowCompileOperation()
    {
        cancel();
        // The executor keeps the completion's shared state alive. The unique public owner must
        // release its Task handle so abandoned UI delivery cannot form a record/state cycle.
        impl_->task = {};
    }
    FlowCompileId FlowCompileOperation::id() const noexcept
    {
        return impl_->id;
    }
    FlowCompileKey FlowCompileOperation::key() const noexcept
    {
        return impl_->key;
    }
    sessions::ObservationVersion FlowCompileOperation::observed() const noexcept
    {
        return impl_->observed;
    }
    process::TaskId FlowCompileOperation::task() const noexcept
    {
        return impl_->attempts.front().task;
    }
    bool FlowCompileOperation::ready() const noexcept
    {
        return impl_->completed.has_value();
    }
    bool FlowCompileOperation::retryable() const noexcept
    {
        return ready() && !*impl_->completed && impl_->object && impl_->attempts.size() < 8;
    }
    std::span<const FlowLinkAttempt> FlowCompileOperation::attempts() const noexcept
    {
        return impl_->attempts;
    }
    Object FlowCompileOperation::object() const noexcept
    {
        return impl_->object;
    }
    FlowCompilationResult<std::shared_ptr<const CompiledFlow>> FlowCompileOperation::result() const
    {
        if (impl_->owner != std::this_thread::get_id())
            return failed(EFlowCompilationError::WRONG_THREAD);
        if (!impl_->completed)
            return failed(EFlowCompilationError::BUSY);
        return *impl_->completed;
    }
    void FlowCompileOperation::cancel() noexcept
    {
        impl_->task.requestStop();
    }
    struct FlowCompilationService::Impl final
    {
        process::ExecutionRuntime& execution;
        const std::thread::id owner{std::this_thread::get_id()};
        std::size_t capacity;
        std::vector<std::unique_ptr<FlowCompileOperation>> operations;
        FlowCompileOperation* find(FlowCompileId id) const noexcept
        {
            auto found = std::ranges::find(operations, id, [](const auto& p) { return p->id(); });
            return found == operations.end() ? nullptr : found->get();
        }
    };
    FlowCompilationService::FlowCompilationService(process::ExecutionRuntime& execution, std::size_t capacity)
        : impl_(std::make_unique<Impl>(execution, std::this_thread::get_id(), capacity))
    {}
    FlowCompilationService::~FlowCompilationService() = default;
    FlowCompilationResult<std::vector<FlowCompileId>> FlowCompilationService::snapshotIds() const
    {
        if (impl_->owner != std::this_thread::get_id())
            return failed(EFlowCompilationError::WRONG_THREAD);
        std::vector<FlowCompileId> result;
        result.reserve(impl_->operations.size());
        for (const auto& operation : impl_->operations)
            result.push_back(operation->id());
        return result;
    }
    FlowCompilationResult<FlowCompileId> FlowCompilationService::start(
        FlowSnapshot source,
        FlowCompileEnvironment env,
        FlowCompileSettings settings,
        LinkSettings link
    )
    {
        if (source.retainedBytes() > settings.byte_limit)
            return failed(EFlowCompilationError::CAPACITY);
        if (impl_->owner != std::this_thread::get_id())
            return failed(EFlowCompilationError::WRONG_THREAD);
        if (impl_->operations.size() >= impl_->capacity)
            return failed(EFlowCompilationError::CAPACITY);
        if (!impl_->execution.blocking())
            return failed(EFlowCompilationError::NO_LINKER);
        if (!settings.version || !settings.byte_limit || !env.version() || !link.version)
            return failed(EFlowCompilationError::INVALID_ID);
        auto state = std::make_shared<FlowCompileOperation::Impl>();
        if (!state->id.value)
            return failed(EFlowCompilationError::CAPACITY);
        state->source = std::make_shared<const lux::flowforge::FlowSource>(source.source());
        state->environment = std::move(env);
        state->settings = settings;
        state->key = {source.content(), settings.version, state->environment.version()};
        state->observed = source.observed();
        state->attempts.push_back({1, link});
        auto submitted = impl_->execution.submit(
            {"Compile Flow", "compiler"},
            [state, link, cpu = impl_->execution.cpu(), blocking = *impl_->execution.blocking()](
                process::TaskReporter reporter
            ) noexcept {
                auto prepared = stdexec::then(stdexec::schedule(cpu), [state, reporter] {
                    reporter.setPhase("Compile Flow");
                    return compile(*state->source, state->environment, state->settings, reporter.stopToken());
                });
                return finish(
                    std::move(prepared),
                    state->source,
                    state->key,
                    state->settings,
                    link,
                    cpu,
                    blocking,
                    reporter
                );
            },
            [state](process::TTaskResult<Completion, VFlowCompilationFailure>&& result) noexcept {
                state->accept(std::move(result));
            }
        );
        if (!submitted)
            return failed(submitted.error());
        state->attempts.back().task = submitted->id();
        state->task = std::move(*submitted);
        const auto id = state->id;
        impl_->operations.push_back(std::unique_ptr<FlowCompileOperation>(new FlowCompileOperation(std::move(state))));
        return id;
    }
    FlowCompilationResult<void> FlowCompilationService::retryLink(FlowCompileId id, LinkSettings link)
    {
        if (impl_->owner != std::this_thread::get_id())
            return failed(EFlowCompilationError::WRONG_THREAD);
        if (!link.version)
            return failed(EFlowCompilationError::INVALID_ID);
        auto* operation = impl_->find(id);
        if (!operation)
            return failed(EFlowCompilationError::INVALID_ID);
        if (!operation->retryable())
            return failed(
                operation->attempts().size() >= 8 ? EFlowCompilationError::CAPACITY : EFlowCompilationError::BUSY
            );
        auto state = operation->impl_;
        auto submitted = impl_->execution.submit(
            {"Retry Flow link", "compiler", operation->task()},
            [state, link, cpu = impl_->execution.cpu(), blocking = *impl_->execution.blocking()](
                process::TaskReporter reporter
            ) noexcept {
                return finish(
                    stdexec::just(ObjectResult{state->object}),
                    state->source,
                    state->key,
                    state->settings,
                    link,
                    cpu,
                    blocking,
                    reporter
                );
            },
            [state](process::TTaskResult<Completion, VFlowCompilationFailure>&& result) noexcept {
                state->accept(std::move(result));
            }
        );
        if (!submitted)
            return failed(submitted.error());
        state->attempts.push_back({state->attempts.size() + 1, std::move(link), submitted->id()});
        state->completed.reset();
        state->task = std::move(*submitted);
        return {};
    }
    FlowCompilationResult<std::reference_wrapper<const FlowCompileOperation>> FlowCompilationService::operation(
        FlowCompileId id
    ) const
    {
        if (impl_->owner != std::this_thread::get_id())
            return failed(EFlowCompilationError::WRONG_THREAD);
        auto* value = impl_->find(id);
        if (!value)
            return failed(EFlowCompilationError::INVALID_ID);
        return std::cref(*value);
    }
    FlowCompilationResult<void> FlowCompilationService::cancel(FlowCompileId id)
    {
        if (impl_->owner != std::this_thread::get_id())
            return failed(EFlowCompilationError::WRONG_THREAD);
        auto* value = impl_->find(id);
        if (!value)
            return failed(EFlowCompilationError::INVALID_ID);
        value->cancel();
        return {};
    }
    FlowCompilationResult<void> FlowCompilationService::acknowledge(FlowCompileId id)
    {
        if (impl_->owner != std::this_thread::get_id())
            return failed(EFlowCompilationError::WRONG_THREAD);
        auto* value = impl_->find(id);
        if (!value)
            return failed(EFlowCompilationError::INVALID_ID);
        if (!value->ready())
            return failed(EFlowCompilationError::BUSY);
        std::erase_if(impl_->operations, [id](const auto& p) { return p->id() == id; });
        return {};
    }
}
