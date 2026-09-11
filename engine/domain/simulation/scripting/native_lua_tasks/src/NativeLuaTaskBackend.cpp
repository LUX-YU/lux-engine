#include <lux/engine/simulation/scripting/native_lua_tasks/NativeLuaTaskBackend.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <vector>

namespace lux::simulation::script
{
struct NativeLuaTaskBackend::Impl final
{
    struct Plan final
    {
        NativeLuaTaskPlan description;
        std::vector<NativeLuaTaskRoute> routes;
        std::vector<lux::rdesc::ScriptFunction> steps;
    };
    struct Instance final
    {
        Impl *owner{};
        const Plan *plan{};
        ScriptInstanceId identity;
        std::uint64_t publication{};
        bool active{};
        ScriptBackendInstance lua;
        ScriptBackendInstance native;
        ResolvedScriptArtifact companion;
        std::vector<PreparedScriptApiCapability> capabilities;
        std::vector<PreparedScriptEventAdmission> events;
        std::vector<ScriptBackendPreparedMethod> step_methods;
        std::vector<PreparedScriptSyncStep> steps;
        ScriptSyncStepSetView step_view;
    };
    struct Method final
    {
        Instance *instance{};
        ScriptBackendPreparedMethod child;
        bool native{};
        bool active{};
    };

    LuaScriptBackend lua;
    CppStaticScriptBackend native;
    ScriptBackendDescriptor lua_api;
    ScriptBackendDescriptor native_api;
    ScriptArtifactResolver artifacts;
    std::vector<Plan> plans;
    std::vector<Instance> instances;
    std::vector<std::size_t> free_instances;
    std::vector<Method> methods;
    std::vector<std::size_t> free_methods;
    std::size_t active_instances{};
    std::size_t active_methods{};
    std::size_t leases{};

    Impl(LuaScriptBackend lua_backend, CppStaticScriptBackend native_backend, const NativeLuaTaskBackendConfig &config)
        : lua(std::move(lua_backend)), native(std::move(native_backend)), lua_api(lua.descriptor()),
          native_api(native.descriptor()), artifacts(config.artifacts)
    {
        plans.reserve(config.plans.size());
        for (const auto &source : config.plans)
        {
            auto &plan = plans.emplace_back();
            plan.description = source;
            plan.routes.assign(source.routes.begin(), source.routes.end());
            plan.steps.assign(source.steps.begin(), source.steps.end());
            plan.description.routes = plan.routes;
            plan.description.steps = plan.steps;
        }
        instances.resize(config.instance_capacity);
        methods.resize(config.prepared_method_capacity);
        free_instances.reserve(instances.size());
        free_methods.reserve(methods.size());
        for (std::size_t i = instances.size(); i > 0U; --i)
            free_instances.push_back(i - 1U);
        for (std::size_t i = methods.size(); i > 0U; --i)
            free_methods.push_back(i - 1U);
    }

    static bool current(const void *opaque, ScriptInstanceId identity, std::uint64_t publication) noexcept
    {
        const auto &instance = *static_cast<const Instance *>(opaque);
        return instance.active && instance.identity == identity && instance.publication == publication;
    }

    void releaseChildren(Instance &instance) noexcept
    {
        instance.active = false;
        if (instance.native)
            native_api.destroyInstance(native_api.context, instance.native);
        instance.native = {};
        instance.step_view = {};
        for (auto it = instance.step_methods.rbegin(); it != instance.step_methods.rend(); ++it)
            lua_api.releaseMethod(lua_api.context, instance.lua, *it);
        instance.step_methods.clear();
        instance.steps.clear();
        if (instance.lua)
            lua_api.destroyInstance(lua_api.context, instance.lua);
        instance.lua = {};
        // Claim release before calling the foreign lease callback.
        const auto companion = instance.companion;
        instance.companion = {};
        if (companion.release)
        {
            --leases;
            companion.release(companion.lease);
        }
        instance.capabilities.clear();
        instance.events.clear();
        instance.plan = nullptr;
    }

    EScriptBackendResult construct(Instance &instance, const ScriptInstanceCreateContext &context,
                                   const lux::script::ScriptArtifact &artifact) noexcept
    {
        const auto match =
            std::ranges::find(plans, context.asset, [](const Plan &plan) { return plan.description.lua_asset; });
        instance.plan = match == plans.end() ? nullptr : &*match;
        if (!instance.plan)
            return lua_api.createInstance(lua_api.context, context, artifact, instance.lua);
        const auto &plan = instance.plan->description;
        const auto *body = std::get_if<lux::rdesc::LuaSourceScript>(&artifact.description().body);
        const bool invalid_lua = body == nullptr || artifact.contentIdentity() != plan.lua_content;
        if (invalid_lua)
            return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
        if (!artifacts.resolve(artifacts.context, plan.native_asset, instance.companion))
            return EScriptBackendResult::CONSTRUCTION_FAILURE;
        if (instance.companion.release)
            ++leases;
        const auto *companion = instance.companion.artifact;
        const bool invalid_native = companion == nullptr || companion->contentIdentity() != plan.native_content ||
                                    !std::holds_alternative<lux::rdesc::CppStaticScript>(companion->description().body);
        if (invalid_native)
            return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
        for (const auto &route : plan.routes)
        {
            const auto *from = artifact.findExport(route.lua_export);
            const auto *to = companion->findExport(route.native_export);
            const auto entry =
                std::ranges::find(plan.contract->exports, route.native_export, &CppStaticExportEntry::symbol);
            const bool invalid_route =
                from == nullptr || to == nullptr || entry == plan.contract->exports.end() || entry->start == nullptr;
            if (invalid_route)
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            const bool invalid_shape =
                from->args != to->args || from->returns != to->returns ||
                !std::ranges::binary_search(body->suspension_capable_exports, route.lua_export) ||
                route.lua_export == artifact.description().lifecycle.begin_play ||
                route.lua_export == artifact.description().lifecycle.end_play;
            if (invalid_shape)
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
        }
        for (const auto &step : plan.steps)
        {
            const auto *actual = artifact.findExport(step.symbol_id);
            if (actual == nullptr || *actual != step ||
                std::ranges::binary_search(body->suspension_capable_exports, step.symbol_id))
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
        }
        // Fallible cold preparation: all arrays stabilize before any child borrows them.
        try
        {
            instance.capabilities.reserve(companion->description().api_requirements.size());
            instance.events.reserve(companion->description().event_requirements.size());
            instance.steps.reserve(plan.steps.size());
            instance.step_methods.reserve(plan.steps.size());
            for (const auto &requirement : companion->description().api_requirements)
            {
                const auto found = std::ranges::find_if(context.capabilities, [&](const auto &capability) {
                    return capability.contract == requirement.contract &&
                           capability.schema_hash == requirement.expected_schema_hash;
                });
                if (found == context.capabilities.end())
                    return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                instance.capabilities.push_back(*found);
            }
            for (const auto &requirement : companion->description().event_requirements)
            {
                const auto found = std::ranges::find_if(
                    context.events, [&](const auto &event) { return event.source && *event.source == requirement; });
                if (found == context.events.end())
                    return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                instance.events.push_back(*found);
            }
        }
        catch (const std::bad_alloc &)
        {
            return EScriptBackendResult::ALLOCATION_FAILURE;
        }
        auto result = lua_api.createInstance(lua_api.context, context, artifact, instance.lua);
        if (result != EScriptBackendResult::SUCCESS)
            return result;
        for (const auto &step : plan.steps)
        {
            ScriptBackendPreparedMethod prepared;
            result = lua.prepareSyncStep(instance.lua, step, prepared);
            if (result != EScriptBackendResult::SUCCESS)
                return result;
            instance.step_methods.push_back(prepared);
            if (prepared.resumable || !prepared.synchronous)
                return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
            instance.steps.push_back({&step, prepared.synchronous});
        }
        instance.step_view = {context.instance, instance.publication, context.behavior, instance.steps, &instance,
                              &current};
        const ScriptInstanceCreateContext native_context{plan.native_asset,  context.scope,         context.behavior,
                                                         context.instance,   instance.capabilities, instance.events,
                                                         &instance.step_view};
        return native_api.createInstance(native_api.context, native_context, *companion, instance.native);
    }

    static EScriptBackendResult createInstance(void *opaque, const ScriptInstanceCreateContext &context,
                                               const lux::script::ScriptArtifact &artifact,
                                               ScriptBackendInstance &output) noexcept
    {
        auto &self = *static_cast<Impl *>(opaque);
        if (self.free_instances.empty())
            return EScriptBackendResult::CAPACITY_EXCEEDED;
        const auto slot = self.free_instances.back();
        auto &instance = self.instances[slot];
        if (instance.publication == (std::numeric_limits<std::uint64_t>::max)())
            return EScriptBackendResult::CAPACITY_EXCEEDED;
        self.free_instances.pop_back();
        instance.owner = &self;
        instance.identity = context.instance;
        ++instance.publication;
        const auto result = self.construct(instance, context, artifact);
        if (result != EScriptBackendResult::SUCCESS)
        {
            self.releaseChildren(instance);
            self.free_instances.push_back(slot);
            return result;
        }
        instance.active = true;
        ++self.active_instances;
        output.value = &instance;
        return EScriptBackendResult::SUCCESS;
    }

    static EScriptBackendResult prepareMethod(void *opaque, ScriptBackendInstance value,
                                              const lux::rdesc::ScriptFunction &function,
                                              ScriptBackendPreparedMethod &output) noexcept
    {
        auto &self = *static_cast<Impl *>(opaque);
        auto *instance = static_cast<Instance *>(value.value);
        if (!instance || instance->owner != &self || !instance->active)
            return EScriptBackendResult::HOST_CONTEXT_MISMATCH;
        if (self.free_methods.empty())
            return EScriptBackendResult::CAPACITY_EXCEEDED;
        const lux::rdesc::ScriptFunction *target = &function;
        bool native = false;
        if (instance->plan)
        {
            const auto &routes = instance->plan->routes;
            const auto route = std::ranges::find(routes, function.symbol_id, &NativeLuaTaskRoute::lua_export);
            if (route != routes.end())
            {
                target = instance->companion.artifact->findExport(route->native_export);
                if (!target || function.args != target->args || function.returns != target->returns)
                    return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
                native = true;
            }
        }
        const auto api = native ? self.native_api : self.lua_api;
        const auto child_instance = native ? instance->native : instance->lua;
        ScriptBackendPreparedMethod child;
        const auto result = api.prepareMethod(api.context, child_instance, *target, child);
        if (result != EScriptBackendResult::SUCCESS)
            return result;
        const auto slot = self.free_methods.back();
        self.free_methods.pop_back();
        auto &method = self.methods[slot];
        method = {instance, child, native, true};
        ++self.active_methods;
        output = {&method, child.synchronous, child.resumable};
        return EScriptBackendResult::SUCCESS;
    }

    static void releaseMethod(void *opaque, ScriptBackendInstance value, ScriptBackendPreparedMethod prepared) noexcept
    {
        auto &self = *static_cast<Impl *>(opaque);
        auto *method = static_cast<Method *>(prepared.token);
        if (!method || !method->active || method->instance != value.value)
            return;
        const auto child = method->child;
        const auto child_instance = method->native ? method->instance->native : method->instance->lua;
        const auto api = method->native ? self.native_api : self.lua_api;
        method->active = false;
        api.releaseMethod(api.context, child_instance, child);
        *method = {};
        --self.active_methods;
        self.free_methods.push_back(static_cast<std::size_t>(method - self.methods.data()));
    }

    static void destroyInstance(void *opaque, ScriptBackendInstance value) noexcept
    {
        auto &self = *static_cast<Impl *>(opaque);
        auto *instance = static_cast<Instance *>(value.value);
        if (!instance || instance->owner != &self || !instance->active)
            return;
        self.releaseChildren(*instance);
        --self.active_instances;
        self.free_instances.push_back(static_cast<std::size_t>(instance - self.instances.data()));
    }
};

NativeLuaTaskBackend::CreateResult NativeLuaTaskBackend::create(NativeLuaTaskBackendConfig config) noexcept
{
    using Error = ENativeLuaTaskBackendError;
    const bool invalid_capacity = config.instance_capacity == 0U || config.prepared_method_capacity == 0U;
    if (invalid_capacity || (!config.plans.empty() && !config.artifacts.resolve))
        return lux::cxx::unexpected(Error::INVALID_CONFIGURATION);
    for (std::size_t i{}; i < config.plans.size(); ++i)
    {
        const auto &plan = config.plans[i];
        if (!plan.contract || plan.routes.empty())
            return lux::cxx::unexpected(Error::INVALID_PLAN);
        const auto pool =
            std::ranges::find(config.native_pools, plan.contract, &CppStaticScriptPoolDescription::descriptor);
        if (pool == config.native_pools.end())
            return lux::cxx::unexpected(Error::INVALID_PLAN);
        for (std::size_t j{}; j < i; ++j)
            if (config.plans[j].lua_asset == plan.lua_asset)
                return lux::cxx::unexpected(Error::DUPLICATE_PLAN);
        for (std::size_t j{}; j < plan.routes.size(); ++j)
            for (std::size_t k{}; k < j; ++k)
                if (plan.routes[j].lua_export == plan.routes[k].lua_export)
                    return lux::cxx::unexpected(Error::INVALID_PLAN);
        for (std::size_t j{}; j < plan.steps.size(); ++j)
            for (std::size_t k{}; k < j; ++k)
                if (plan.steps[j].symbol_id == plan.steps[k].symbol_id)
                    return lux::cxx::unexpected(Error::INVALID_PLAN);
    }
    auto lua = LuaScriptBackend::create(config.lua);
    if (!lua)
        return lux::cxx::unexpected(Error::LUA_BACKEND_FAILURE);
    auto native = CppStaticScriptBackend::create(config.native_pools);
    if (!native)
        return lux::cxx::unexpected(Error::NATIVE_BACKEND_FAILURE);
    try
    {
        return NativeLuaTaskBackend(std::make_unique<Impl>(std::move(*lua), std::move(*native), config));
    }
    catch (const std::bad_alloc &)
    {
        return lux::cxx::unexpected(Error::ALLOCATION_FAILURE);
    }
}

NativeLuaTaskBackend::NativeLuaTaskBackend(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}
NativeLuaTaskBackend::~NativeLuaTaskBackend() = default;
NativeLuaTaskBackend::NativeLuaTaskBackend(NativeLuaTaskBackend &&) noexcept = default;
NativeLuaTaskBackend &NativeLuaTaskBackend::operator=(NativeLuaTaskBackend &&) noexcept = default;
ScriptBackendDescriptor NativeLuaTaskBackend::descriptor() noexcept
{
    return {lux::rdesc::Script::Kind::LUA, impl_.get(),          &Impl::createInstance,
            &Impl::prepareMethod,          &Impl::releaseMethod, &Impl::destroyInstance};
}
NativeLuaTaskBackendStats NativeLuaTaskBackend::stats() const noexcept
{
    if (!impl_)
        return {};
    const auto &self = *impl_;
    std::size_t backing = sizeof(Impl) + self.instances.capacity() * sizeof(Impl::Instance) +
                          self.methods.capacity() * sizeof(Impl::Method) + self.plans.capacity() * sizeof(Impl::Plan) +
                          (self.free_instances.capacity() + self.free_methods.capacity()) * sizeof(std::size_t);
    for (const auto &instance : self.instances)
        backing += instance.capabilities.capacity() * sizeof(PreparedScriptApiCapability) +
                   instance.events.capacity() * sizeof(PreparedScriptEventAdmission) +
                   instance.steps.capacity() * sizeof(PreparedScriptSyncStep) +
                   instance.step_methods.capacity() * sizeof(ScriptBackendPreparedMethod);
    for (const auto &plan : self.plans)
    {
        backing += plan.routes.capacity() * sizeof(NativeLuaTaskRoute) +
                   plan.steps.capacity() * sizeof(lux::rdesc::ScriptFunction);
        for (const auto &step : plan.steps)
            backing += step.name.capacity() + 1U +
                       (step.args.capacity() + step.returns.capacity()) * sizeof(lux::rdesc::ScriptValueType);
    }
    return {self.active_instances, self.active_methods, self.leases, backing, self.lua.stats(), self.native.stats()};
}
} // namespace lux::simulation::script
