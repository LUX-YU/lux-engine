#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <unordered_map>
#include <vector>

namespace lux::simulation::script
{
lux::script::ScriptAbilityCoroutine<DelayAbility, ScriptCoroutineContext> ScriptCoroutineContext::delay() noexcept
{
    const auto slot = findAbility(lux::script::ScriptAbilityTraits<DelayAbility>::Description.id.hash());
    return {*this, slot.value_or((std::numeric_limits<std::uint32_t>::max)())};
}

namespace
{
bool sameValue(const lux::rdesc::ScriptValueType &value, const CppStaticValueView &expected) noexcept
{
    return value.canonical_name == expected.layout.canonical_name && value.type_id == expected.layout.type_id &&
           value.pass == expected.pass && value.abi_kind == expected.layout.abi_kind &&
           value.size == expected.layout.size && value.alignment == expected.layout.alignment;
}

bool sameFunction(const lux::rdesc::ScriptFunction &function, const CppStaticExportEntry &entry) noexcept
{
    if (function.symbol_id != entry.symbol || function.args.size() != entry.parameters.size() ||
        function.returns.size() != entry.results.size())
        return false;
    for (std::size_t index{}; index < function.args.size(); ++index)
        if (!sameValue(function.args[index], entry.parameters[index]))
            return false;
    for (std::size_t index{}; index < function.returns.size(); ++index)
        if (!sameValue(function.returns[index], entry.results[index]))
            return false;
    return true;
}

bool validContract(const CppStaticContract &contract) noexcept
{
    const auto &object = contract.object;
    const bool invalid_object =
        object.size != 0U && (object.alignment == 0U || (object.alignment & (object.alignment - 1U)) != 0U ||
                              object.construct == nullptr || object.destroy == nullptr);
    if (contract.key.empty() || contract.module.empty() || contract.exports.empty() || invalid_object)
        return false;
    bool begin_found = contract.lifecycle.begin_play == lux::script::InvalidScriptSymbolId;
    bool end_found = contract.lifecycle.end_play == lux::script::InvalidScriptSymbolId;
    for (const auto &entry : contract.exports)
    {
        const bool missing_entry = (entry.invoke == nullptr) == (entry.start == nullptr);
        if (entry.symbol == lux::script::InvalidScriptSymbolId || entry.name.empty() || missing_entry)
            return false;
        if (entry.symbol == contract.lifecycle.begin_play)
        {
            if (entry.start != nullptr || !entry.parameters.empty() || !entry.results.empty())
                return false;
            begin_found = true;
        }
        if (entry.symbol == contract.lifecycle.end_play)
        {
            constexpr auto reason = detail::cppStaticValue<EScriptEndPlayReason>();
            const bool invalid_end = entry.start != nullptr || entry.parameters.size() != 1U || !entry.results.empty();
            if (invalid_end || entry.parameters[0].layout.type_id != reason.layout.type_id ||
                entry.parameters[0].pass != reason.pass)
                return false;
            end_found = true;
        }
    }
    return begin_found && end_found;
}

bool executableContractMatches(const lux::script::ScriptArtifact &artifact, const CppStaticContract &contract) noexcept
{
    const auto &description = artifact.description();
    const auto *body = std::get_if<lux::rdesc::CppStaticScript>(&description.body);
    const bool invalid =
        body == nullptr || body->descriptor != contract.key || description.module_name != contract.module ||
        description.exports.size() != contract.exports.size() || description.lifecycle != contract.lifecycle ||
        description.api_requirements.size() != contract.abilities.size() ||
        description.event_requirements.size() != contract.events.size();
    if (invalid)
        return false;
    std::size_t coroutine_count{};
    for (const auto &entry : contract.exports)
    {
        const auto *function = artifact.findExport(entry.symbol);
        if (function == nullptr || !sameFunction(*function, entry))
            return false;
        if (entry.start != nullptr)
        {
            ++coroutine_count;
            if (!std::ranges::binary_search(body->suspension_capable_exports, entry.symbol))
                return false;
        }
    }
    if (body->suspension_capable_exports.size() != coroutine_count)
        return false;
    for (const auto &event : contract.events)
    {
        if (std::ranges::none_of(description.event_requirements,
                                 [&](const auto &candidate) noexcept { return event.matches(candidate); }))
            return false;
    }
    for (const auto &requirement : contract.abilities)
    {
        const auto found = std::ranges::find_if(description.api_requirements, [&](const auto &candidate) noexcept {
            return candidate.contract.hash() == requirement.contract.hash() &&
                   candidate.contract.name() == requirement.contract.name() &&
                   candidate.expected_schema_hash == requirement.expected_schema_hash;
        });
        if (found == description.api_requirements.end())
            return false;
    }
    return true;
}
} // namespace

CppStaticDescriptionResult materializeCppStaticScript(const CppStaticContract &contract) noexcept
{
    if (!validContract(contract))
        return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);
    try
    {
        lux::rdesc::Script result;
        result.module_name = contract.module;
        result.lifecycle = contract.lifecycle;
        lux::rdesc::CppStaticScript body;
        body.descriptor = contract.key;
        const auto value = [](const CppStaticValueView &type) {
            return lux::rdesc::ScriptValueType{std::string(type.layout.canonical_name),
                                               type.layout.type_id,
                                               type.pass,
                                               type.layout.abi_kind,
                                               type.layout.size,
                                               type.layout.alignment};
        };
        result.exports.reserve(contract.exports.size());
        for (const auto &entry : contract.exports)
        {
            lux::rdesc::ScriptFunction function;
            function.name = entry.name;
            function.symbol_id = entry.symbol;
            for (const auto &parameter : entry.parameters)
                function.args.push_back(value(parameter));
            for (const auto &returned : entry.results)
                function.returns.push_back(value(returned));
            result.exports.push_back(std::move(function));
            if (entry.start != nullptr)
                body.suspension_capable_exports.push_back(entry.symbol);
        }
        for (const auto &requirement : contract.abilities)
            result.api_requirements.push_back(
                {lux::script::ScriptApiContractId{requirement.contract.name()}, requirement.expected_schema_hash});
        std::ranges::sort(body.suspension_capable_exports);
        for (const auto &event : contract.events)
            result.event_requirements.push_back(event.materialize());
        std::ranges::sort(result.event_requirements, lux::script::ScriptEventSourceLess{});
        result.body = std::move(body);
        if (!lux::rdesc::validScriptDescription(result))
            return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);
        return result;
    }
    catch (const std::bad_alloc &)
    {
        return lux::cxx::unexpected(ECppStaticScriptBridgeError::ALLOCATION_FAILURE);
    }
}

struct CppStaticScriptBackend::State final
{
    struct ObjectSlab final
    {
        ObjectSlab() noexcept = default;
        ObjectSlab(const ObjectSlab &) = delete;
        ObjectSlab &operator=(const ObjectSlab &) = delete;

        ObjectSlab(ObjectSlab &&other) noexcept
            : data(std::exchange(other.data, nullptr)), alignment(std::exchange(other.alignment, 0U))
        {
        }

        ObjectSlab &operator=(ObjectSlab &&other) noexcept
        {
            if (this == std::addressof(other))
                return *this;
            reset();
            data = std::exchange(other.data, nullptr);
            alignment = std::exchange(other.alignment, 0U);
            return *this;
        }

        ~ObjectSlab()
        {
            reset();
        }

        void reset() noexcept
        {
            if (data)
                ::operator delete(data, std::align_val_t{alignment});
            data = nullptr;
            alignment = 0U;
        }

        void *data{};
        std::size_t alignment{};
    };

    struct DescriptorIndex final
    {
        const CppStaticContract *descriptor{};
        std::unordered_map<lux::script::ScriptSymbolId, const CppStaticExportEntry *> callables;
        ObjectSlab objects;
        std::vector<std::size_t> free_objects;
        detail::BoundedClassStorage coroutine_frames;
        std::size_t object_stride{};
        std::size_t instance_capacity{};
        std::size_t active_instances{};
        std::size_t coroutine_capacity{};
        std::size_t active_coroutines{};
        std::size_t coroutine_high_water{};
    };

    struct Instance final
    {
        State *owner{};
        DescriptorIndex *descriptor{};
        void *object{};
        std::size_t object_slot{(std::numeric_limits<std::size_t>::max)()};
        std::uint32_t slot{};
        std::span<const PreparedScriptApiCapability> capabilities;
        std::size_t active_coroutines{};
    };

    struct CoroutineContinuation final
    {
        State *owner{};
        DescriptorIndex *descriptor{};
        Instance *instance{};
        ScriptCoroutineContext context;
        detail::BoundedClassStorage::Allocation arguments;
        std::coroutine_handle<ScriptCoroutine::promise_type> handle;
        std::uint32_t slot{};
        bool active{};
    };

    struct PreparedCall final
    {
        Instance *instance{};
        const CppStaticExportEntry *callable{};
        detail::BoundedClassStorage::ClassHandle argument_class;
        bool active{};

        static ScriptStepResult invokeStep(void *opaque, lux_script_call_frame &frame, ScriptStepContext &step,
                                           ScriptBackendContinuation &result) noexcept
        {
            auto &self = *static_cast<PreparedCall *>(opaque);
            const bool invalid =
                !self.active || self.instance == nullptr || self.callable == nullptr || self.callable->start == nullptr;
            if (invalid)
                return ScriptStepResult::failed(-1);
            auto &state = *self.instance->owner;
            auto *continuation = state.acquireCoroutine(*self.instance);
            if (continuation == nullptr)
                return ScriptStepResult::failed(-1);
            if (self.callable->owned_bytes != 0U)
            {
                auto storage =
                    continuation->descriptor->coroutine_frames.acquire(self.argument_class, self.callable->owned_bytes);
                if (!storage)
                {
                    state.releaseCoroutineSlot(*continuation);
                    return ScriptStepResult::failed(-1);
                }
                continuation->arguments = *storage;
            }
            CppStaticCoroutineAccess::activate(continuation->context, step);
            auto coroutine =
                self.callable->start(self.instance->object, continuation->context, frame, continuation->arguments.data);
            CppStaticCoroutineAccess::deactivate(continuation->context);
            if (!coroutine)
            {
                state.releaseCoroutineSlot(*continuation);
                return ScriptStepResult::failed(-1);
            }
            continuation->handle = CppStaticCoroutineAccess::release(coroutine);
            const auto outcome = continuation->handle.promise().outcome;
            const bool suspended =
                !continuation->handle.done() && outcome.state == EScriptStepState::SUSPENDED && outcome.valid();
            if (suspended)
            {
                result = {continuation, &State::resumeCoroutine, &State::destroyCoroutineErased};
                return outcome;
            }
            const bool completed =
                continuation->handle.done() && outcome.state == EScriptStepState::COMPLETED && outcome.valid();
            state.destroyCoroutine(*continuation);
            return completed ? outcome : ScriptStepResult::failed(-1);
        }
    };

    explicit State(std::span<const CppStaticScriptPoolDescription> pools)
    {
        descriptor_indexes.reserve(pools.size());
        descriptor_by_key.reserve(pools.size());
        std::size_t prepared_capacity{};
        std::size_t coroutine_capacity{};
        for (const auto &pool : pools)
        {
            const auto *descriptor = pool.descriptor;
            if (!descriptor || !validContract(*descriptor) || pool.instance_capacity == 0U)
            {
                valid = false;
                return;
            }
            const bool instance_capacity_overflow =
                instance_capacity > (std::numeric_limits<std::size_t>::max)() - pool.instance_capacity;
            const bool prepared_capacity_overflow =
                pool.prepared_method_capacity == 0U ||
                prepared_capacity > (std::numeric_limits<std::size_t>::max)() - pool.prepared_method_capacity;
            const bool coroutine_capacity_overflow =
                coroutine_capacity > (std::numeric_limits<std::size_t>::max)() - pool.coroutine_capacity;
            if (instance_capacity_overflow || prepared_capacity_overflow || coroutine_capacity_overflow)
            {
                valid = false;
                return;
            }

            DescriptorIndex index;
            index.descriptor = descriptor;
            index.instance_capacity = pool.instance_capacity;
            index.coroutine_capacity = pool.coroutine_capacity;
            const bool has_coroutines = std::ranges::any_of(
                descriptor->exports, [](const auto &entry) noexcept { return entry.start != nullptr; });
            if (has_coroutines)
            {
                if (pool.coroutine_capacity > (std::numeric_limits<std::size_t>::max)() / 2U)
                {
                    valid = false;
                    return;
                }
                auto frames = detail::BoundedClassStorage::create(
                    pool.coroutine_frame_classes, pool.coroutine_frame_storage_bytes, pool.coroutine_capacity * 2U);
                if (!frames)
                {
                    error = frames.error() == detail::EClassStorageError::ALLOCATION_FAILURE
                                ? ECppStaticScriptBridgeError::ALLOCATION_FAILURE
                                : ECppStaticScriptBridgeError::INVALID_DESCRIPTOR;
                    valid = false;
                    return;
                }
                index.coroutine_frames = std::move(*frames);
            }
            else if (pool.coroutine_capacity != 0U || pool.coroutine_frame_storage_bytes != 0U)
            {
                valid = false;
                return;
            }
            index.callables.reserve(index.descriptor->exports.size());
            for (const auto &callable : index.descriptor->exports)
            {
                if (!index.callables.emplace(callable.symbol, std::addressof(callable)).second)
                {
                    valid = false;
                    return;
                }
            }
            if (index.descriptor->object.size)
            {
                const auto &type = index.descriptor->object;
                const bool valid_alignment = type.alignment != 0U && (type.alignment & (type.alignment - 1U)) == 0U;
                const bool stride_overflow =
                    valid_alignment && type.size > (std::numeric_limits<std::size_t>::max)() - (type.alignment - 1U);
                if (type.size == 0U || !valid_alignment || stride_overflow)
                {
                    valid = false;
                    return;
                }
                index.object_stride = (type.size + type.alignment - 1U) & ~(type.alignment - 1U);
                if (pool.instance_capacity > (std::numeric_limits<std::size_t>::max)() / index.object_stride)
                {
                    valid = false;
                    return;
                }
                const auto slab_size = index.object_stride * pool.instance_capacity;
                index.objects.data = ::operator new(slab_size, std::align_val_t{type.alignment}, std::nothrow);
                if (!index.objects.data)
                {
                    error = ECppStaticScriptBridgeError::ALLOCATION_FAILURE;
                    valid = false;
                    return;
                }
                index.objects.alignment = type.alignment;
                index.free_objects.reserve(pool.instance_capacity);
                for (std::size_t slot = pool.instance_capacity; slot > 0U; --slot)
                    index.free_objects.push_back(slot - 1U);
            }
            descriptor_indexes.push_back(std::move(index));
            const auto descriptor_index = descriptor_indexes.size() - 1U;
            if (!descriptor_by_key.emplace(descriptor->key, descriptor_index).second)
            {
                valid = false;
                return;
            }
            instance_capacity += pool.instance_capacity;
            prepared_capacity += pool.prepared_method_capacity;
            coroutine_capacity += pool.coroutine_capacity;
        }
        instances.resize(instance_capacity);
        free_instances.reserve(instance_capacity);
        for (std::size_t index = instance_capacity; index > 0U; --index)
            free_instances.push_back(index - 1U);
        prepared_calls.resize(prepared_capacity);
        free_prepared_calls.reserve(prepared_capacity);
        for (std::size_t index = prepared_capacity; index > 0U; --index)
        {
            free_prepared_calls.push_back(index - 1U);
        }
        continuations.resize(coroutine_capacity);
        free_continuations.reserve(coroutine_capacity);
        for (std::size_t index = coroutine_capacity; index > 0U; --index)
            free_continuations.push_back(index - 1U);
    }

    ~State()
    {
        for (auto &continuation : continuations)
        {
            if (continuation.active && continuation.handle)
                destroyCoroutine(continuation);
        }
        for (auto &instance : instances)
        {
            if (instance.object && instance.descriptor && instance.descriptor->descriptor->object.destroy)
            {
                instance.descriptor->descriptor->object.destroy(instance.object);
            }
        }
    }

    [[nodiscard]] static bool findAbility(void *opaque, std::uint32_t instance_slot, std::uint64_t contract_hash,
                                          std::uint32_t &result) noexcept
    {
        auto &self = *static_cast<State *>(opaque);
        if (instance_slot >= self.instances.size())
            return false;
        const auto &instance = self.instances[instance_slot];
        if (instance.descriptor == nullptr)
            return false;
        for (std::size_t index{}; index < instance.capabilities.size(); ++index)
        {
            if (instance.capabilities[index].contract.hash() == contract_hash)
            {
                result = static_cast<std::uint32_t>(index);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static bool resolveAbility(void *opaque, std::uint32_t instance_slot, std::uint32_t ability_slot,
                                             detail::ScriptCoroutineAbilityAccess &result) noexcept
    {
        auto &self = *static_cast<State *>(opaque);
        if (instance_slot >= self.instances.size())
            return false;
        const auto &instance = self.instances[instance_slot];
        if (instance.descriptor == nullptr || ability_slot >= instance.capabilities.size())
            return false;
        const auto &ability = instance.capabilities[ability_slot];
        if (ability.context == nullptr || ability.dispatch == nullptr)
            return false;
        result = {ability.context, ability.dispatch};
        return true;
    }

    void releaseCoroutineSlot(CoroutineContinuation &continuation) noexcept
    {
        auto *descriptor = continuation.descriptor;
        auto *instance = continuation.instance;
        const auto slot = continuation.slot;
        if (continuation.arguments && descriptor != nullptr)
            static_cast<void>(descriptor->coroutine_frames.release(continuation.arguments));
        continuation = {};
        if (descriptor != nullptr)
            --descriptor->active_coroutines;
        if (instance != nullptr)
            --instance->active_coroutines;
        free_continuations.push_back(slot);
    }

    void destroyCoroutine(CoroutineContinuation &continuation) noexcept
    {
        if (!continuation.active)
            return;
        if (continuation.handle)
            continuation.handle.destroy();
        releaseCoroutineSlot(continuation);
    }

    [[nodiscard]] CoroutineContinuation *acquireCoroutine(Instance &instance) noexcept
    {
        auto &descriptor = *instance.descriptor;
        if (free_continuations.empty() || descriptor.active_coroutines >= descriptor.coroutine_capacity)
            return nullptr;
        const auto slot = free_continuations.back();
        free_continuations.pop_back();
        auto &continuation = continuations[slot];
        continuation.owner = this;
        continuation.descriptor = std::addressof(descriptor);
        continuation.instance = std::addressof(instance);
        continuation.context = CppStaticCoroutineAccess::context(this, instance.slot, &State::findAbility,
                                                                 &State::resolveAbility, descriptor.coroutine_frames);
        continuation.slot = static_cast<std::uint32_t>(slot);
        continuation.active = true;
        ++descriptor.active_coroutines;
        descriptor.coroutine_high_water = (std::max)(descriptor.coroutine_high_water, descriptor.active_coroutines);
        ++instance.active_coroutines;
        return std::addressof(continuation);
    }

    [[nodiscard]] static ScriptStepResult resumeCoroutine(void *opaque, ScriptStepContext &step,
                                                          const ScriptResumePacket &packet) noexcept
    {
        auto &continuation = *static_cast<CoroutineContinuation *>(opaque);
        if (!continuation.active || !continuation.handle || continuation.instance == nullptr)
            return ScriptStepResult::failed(-1);
        auto &promise = continuation.handle.promise();
        if (!promise.prepareResume(packet))
            return promise.outcome;
        CppStaticCoroutineAccess::activate(continuation.context, step, std::addressof(packet));
        continuation.handle.resume();
        CppStaticCoroutineAccess::deactivate(continuation.context);
        promise.clearResume();
        const auto result = promise.outcome;
        const bool is_invalid_terminal = continuation.handle.done() && result.state == EScriptStepState::SUSPENDED;
        const bool is_invalid_suspension = !continuation.handle.done() && result.state != EScriptStepState::SUSPENDED;
        return is_invalid_terminal || is_invalid_suspension ? ScriptStepResult::failed(-1) : result;
    }

    static void destroyCoroutineErased(void *opaque) noexcept
    {
        auto *continuation = static_cast<CoroutineContinuation *>(opaque);
        if (continuation != nullptr && continuation->owner != nullptr)
            continuation->owner->destroyCoroutine(*continuation);
    }

    [[nodiscard]] DescriptorIndex *find(const lux::script::ScriptArtifact &artifact) noexcept
    {
        const auto *body = std::get_if<lux::rdesc::CppStaticScript>(std::addressof(artifact.description().body));
        if (!body)
            return nullptr;
        const auto found = descriptor_by_key.find(body->descriptor);
        return found == descriptor_by_key.end() ? nullptr : std::addressof(descriptor_indexes[found->second]);
    }

    static EScriptBackendResult createInstance(void *opaque, const ScriptInstanceCreateContext &context,
                                               const lux::script::ScriptArtifact &artifact,
                                               ScriptBackendInstance &result) noexcept
    {
        auto &self = *static_cast<State *>(opaque);
        auto *descriptor_index = self.find(artifact);
        if (!descriptor_index)
            return EScriptBackendResult::CONSTRUCTION_FAILURE;
        const auto &descriptor = *descriptor_index->descriptor;
        if (!executableContractMatches(artifact, descriptor))
        {
            return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
        }
        const bool entity_scope = std::holds_alternative<EntityScriptScope>(context.scope);
        if (descriptor.entity_scope != entity_scope)
            return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
        if (self.free_instances.empty() || descriptor_index->active_instances >= descriptor_index->instance_capacity)
            return EScriptBackendResult::CAPACITY_EXCEEDED;
        if (descriptor.object.size != 0U && descriptor_index->free_objects.empty())
            return EScriptBackendResult::CAPACITY_EXCEEDED;
        const auto instance_slot = self.free_instances.back();
        self.free_instances.pop_back();
        auto *instance = std::addressof(self.instances[instance_slot]);
        instance->owner = std::addressof(self);
        instance->descriptor = descriptor_index;
        instance->object = nullptr;
        instance->slot = static_cast<std::uint32_t>(instance_slot);
        instance->capabilities = context.capabilities;
        if (descriptor.object.size != 0U)
        {
            instance->object_slot = descriptor_index->free_objects.back();
            descriptor_index->free_objects.pop_back();
            instance->object = static_cast<std::byte *>(descriptor_index->objects.data) +
                               instance->object_slot * descriptor_index->object_stride;
            if (!descriptor.object.construct(instance->object))
            {
                descriptor_index->free_objects.push_back(instance->object_slot);
                *instance = {};
                self.free_instances.push_back(instance_slot);
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            }
            if (descriptor.object.attach && context.behavior)
                descriptor.object.attach(instance->object, *context.behavior);
        }
        ++descriptor_index->active_instances;
        ++self.active_instances;
        result.value = instance;
        return EScriptBackendResult::SUCCESS;
    }

    static EScriptBackendResult prepareMethod(void *opaque, ScriptBackendInstance opaque_instance,
                                              const lux::rdesc::ScriptFunction &function,
                                              ScriptBackendPreparedMethod &result) noexcept
    {
        auto &self = *static_cast<State *>(opaque);
        auto *instance = static_cast<Instance *>(opaque_instance.value);
        if (!instance)
            return EScriptBackendResult::CONSTRUCTION_FAILURE;
        const auto found = instance->descriptor->callables.find(function.symbol_id);
        if (found == instance->descriptor->callables.end())
            return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
        if (!sameFunction(function, *found->second))
            return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
        const auto argument_class = found->second->owned_bytes != 0U
                                        ? instance->descriptor->coroutine_frames.select(found->second->owned_bytes,
                                                                                        found->second->owned_alignment)
                                        : detail::BoundedClassStorage::ClassHandle{};
        if (found->second->owned_bytes != 0U && !argument_class)
            return EScriptBackendResult::CAPACITY_EXCEEDED;
        if (self.free_prepared_calls.empty())
            return EScriptBackendResult::CAPACITY_EXCEEDED;
        const auto call_slot = self.free_prepared_calls.back();
        self.free_prepared_calls.pop_back();
        auto &call = self.prepared_calls[call_slot];
        call.instance = instance;
        call.callable = found->second;
        call.active = true;
        call.argument_class = argument_class;
        result = {std::addressof(call),
                  call.callable->invoke != nullptr
                      ? lux::script::BoundScriptCall{call.callable->invoke, instance->object}
                      : lux::script::BoundScriptCall{},
                  call.callable->start != nullptr ? BoundScriptStepCall{std::addressof(call), &PreparedCall::invokeStep}
                                                  : BoundScriptStepCall{}};
        return EScriptBackendResult::SUCCESS;
    }

    static void releaseMethod(void *opaque, ScriptBackendInstance, ScriptBackendPreparedMethod method) noexcept
    {
        auto &self = *static_cast<State *>(opaque);
        auto *prepared = static_cast<PreparedCall *>(method.token);
        if (!prepared || !prepared->active)
            return;
        prepared->instance = nullptr;
        prepared->callable = nullptr;
        prepared->active = false;
        const auto index = static_cast<std::size_t>(prepared - self.prepared_calls.data());
        self.free_prepared_calls.push_back(index);
    }

    static void destroyInstance(void *opaque, ScriptBackendInstance opaque_instance) noexcept
    {
        auto &self = *static_cast<State *>(opaque);
        auto *instance = static_cast<Instance *>(opaque_instance.value);
        if (!instance)
            return;
        if (instance->active_coroutines != 0U)
            std::terminate();
        if (instance->object)
        {
            instance->descriptor->descriptor->object.destroy(instance->object);
            instance->descriptor->free_objects.push_back(instance->object_slot);
        }
        --instance->descriptor->active_instances;
        const auto index = static_cast<std::size_t>(instance - self.instances.data());
        *instance = {};
        self.free_instances.push_back(index);
        --self.active_instances;
    }

    std::vector<DescriptorIndex> descriptor_indexes;
    std::unordered_map<std::string_view, std::size_t> descriptor_by_key;
    std::vector<Instance> instances;
    std::vector<std::size_t> free_instances;
    std::vector<PreparedCall> prepared_calls;
    std::vector<std::size_t> free_prepared_calls;
    std::vector<CoroutineContinuation> continuations;
    std::vector<std::size_t> free_continuations;
    std::size_t instance_capacity{};
    std::size_t active_instances{};
    ECppStaticScriptBridgeError error{ECppStaticScriptBridgeError::INVALID_DESCRIPTOR};
    bool valid{true};
};

lux::cxx::expected<CppStaticScriptBackend, ECppStaticScriptBridgeError> CppStaticScriptBackend::create(
    std::span<const CppStaticScriptPoolDescription> pools) noexcept
{
    if (pools.empty())
        return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);
    try
    {
        auto state = std::make_unique<State>(pools);
        if (!state->valid)
            return lux::cxx::unexpected(state->error);
        return CppStaticScriptBackend{std::move(state)};
    }
    catch (const std::bad_alloc &)
    {
        return lux::cxx::unexpected(ECppStaticScriptBridgeError::ALLOCATION_FAILURE);
    }
}

CppStaticScriptBackend::CppStaticScriptBackend(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

CppStaticScriptBackend::~CppStaticScriptBackend() = default;
CppStaticScriptBackend::CppStaticScriptBackend(CppStaticScriptBackend &&) noexcept = default;
CppStaticScriptBackend &CppStaticScriptBackend::operator=(CppStaticScriptBackend &&) noexcept = default;

CppStaticScriptBackend::operator bool() const noexcept
{
    return state_ != nullptr;
}

CppStaticScriptBackendStats CppStaticScriptBackend::stats() const noexcept
{
    CppStaticScriptBackendStats result;
    if (!state_)
        return result;
    result.prepared_method_storage_bytes = state_->prepared_calls.capacity() * sizeof(State::PreparedCall) +
                                           state_->free_prepared_calls.capacity() * sizeof(std::size_t);
    result.active_prepared_methods = state_->prepared_calls.size() - state_->free_prepared_calls.size();
    for (const auto &descriptor : state_->descriptor_indexes)
    {
        const auto stats = descriptor.coroutine_frames.stats();
        result.frame_storage_bytes += stats.arena_bytes;
        result.active_frames += descriptor.active_coroutines;
        result.frame_high_water += descriptor.coroutine_high_water;
        result.frame_capacity_failures += stats.capacity_failures;
    }
    return result;
}

ScriptBackendDescriptor CppStaticScriptBackend::descriptor() noexcept
{
    return state_ ? ScriptBackendDescriptor{lux::rdesc::Script::Kind::CPP_STATIC,
                                            state_.get(),
                                            &State::createInstance,
                                            &State::prepareMethod,
                                            &State::releaseMethod,
                                            &State::destroyInstance}
                  : ScriptBackendDescriptor{};
}
} // namespace lux::simulation::script
