#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>
#include <lux/engine/simulation/scripting/native/NativeScriptAbilityProjection.hpp>
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/ScriptContractValidation.hpp>
#include <lux/engine/simulation/scripting/detail/BoundedFrameStorage.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::simulation::script
{
    struct NativeScriptBackend::State final
    {
        struct ModuleEntry final
        {
            lux::asset::AssetId asset;
            const lux::script::NativeModule* module{};
            void* lease{};
            void (*release)(void*) noexcept{};
            void* state_slab{};
            std::size_t state_stride{};
            std::size_t state_align{1U};
            bool over_aligned{};
            std::vector<std::size_t> free_state_slots;
        };

        struct Instance final
        {
            struct PreparedEvent final
            {
                const lux::script::ScriptEventSourceDescription* source{};
                const lux_script_event_wait_import_desc* import{};
            };

            ModuleEntry* module{};
            void* state{};
            std::size_t state_size{};
            std::size_t state_align{1U};
            bool over_aligned{};
            std::size_t state_slot{(std::numeric_limits<std::size_t>::max)()};
            std::vector<lux_script_prepared_ability> abilities;
            std::vector<PreparedEvent> events;
            lux_script_native_instance_context native_context{};
        };

        struct PreparedCall final
        {
            State* owner{};
            Instance* instance{};
            const lux_script_function_desc* function{};
        };

        struct NativeContinuation final
        {
            State* owner{};
            PreparedCall* call{};
            detail::BoundedFrameStorage::Allocation frame;
            std::size_t slot{(std::numeric_limits<std::size_t>::max)()};
            const lux_script_type_desc* waiting_event_payload{};
        };

        struct StepAdapter final
        {
            PreparedCall* call{};
            ScriptStepContext* context{};
            NativeContinuation* continuation{};
        };

        State(
            NativeModuleResolver source_resolver,
            NativeScriptBackendConfig source_config,
            detail::BoundedFrameStorage source_frame_storage
        )
            : resolver(source_resolver),
              config(source_config),
              module_capacity(source_config.module_capacity),
              instance_capacity(source_config.instance_capacity),
              record_layouts(source_config.record_layouts),
              ability_contributions(source_config.abilities.begin(), source_config.abilities.end()),
              frame_storage(std::move(source_frame_storage))
        {
            modules.reserve(module_capacity);
            module_index.reserve(module_capacity);
            instances.resize(instance_capacity);
            free_instances.reserve(instance_capacity);
            for (std::size_t index = instance_capacity; index > 0U; --index)
                free_instances.push_back(index - 1U);
            prepared_calls.resize(config.prepared_call_capacity);
            free_prepared_calls.reserve(config.prepared_call_capacity);
            for (std::size_t index = config.prepared_call_capacity; index > 0U; --index)
                free_prepared_calls.push_back(index - 1U);
            continuations.resize(config.continuation_capacity);
            free_continuations.reserve(config.continuation_capacity);
            for (std::size_t index = config.continuation_capacity; index > 0U; --index)
                free_continuations.push_back(index - 1U);
        }

        ~State()
        {
            for (auto& continuation : continuations)
            {
                if (continuation.frame)
                    destroyNativeContinuation(continuation);
            }
            for (auto entry = modules.rbegin(); entry != modules.rend(); ++entry)
            {
                if (entry->state_slab)
                {
                    if (entry->over_aligned)
                    {
                        ::operator delete(
                            entry->state_slab,
                            std::align_val_t{entry->state_align}
                        );
                    }
                    else
                    {
                        ::operator delete(entry->state_slab);
                    }
                }
                if (entry->release)
                    entry->release(entry->lease);
            }
        }

        [[nodiscard]] bool initializeStateSlab(
            ModuleEntry& entry,
            const lux::rdesc::NativeModuleScript& body
        ) noexcept
        {
            if (body.state_size == 0U)
                return true;
            const auto alignment = static_cast<std::size_t>(body.state_align);
            const auto size = static_cast<std::size_t>(body.state_size);
            const auto padding = alignment - 1U;
            const bool stride_overflow = size >
                (std::numeric_limits<std::size_t>::max)() - padding;
            if (stride_overflow)
                return false;
            entry.state_stride = (size + padding) & ~padding;
            const bool slab_overflow = instance_capacity != 0U &&
                entry.state_stride >
                    (std::numeric_limits<std::size_t>::max)() /
                        instance_capacity;
            if (slab_overflow)
                return false;
            entry.state_align = alignment;
            entry.over_aligned = alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
            const auto slab_size = entry.state_stride * instance_capacity;
            entry.state_slab = entry.over_aligned
                ? ::operator new(
                    slab_size,
                    std::align_val_t{alignment},
                    std::nothrow
                )
                : ::operator new(slab_size, std::nothrow);
            if (!entry.state_slab)
                return false;
            try
            {
                entry.free_state_slots.reserve(instance_capacity);
                for (std::size_t index = instance_capacity; index > 0U; --index)
                    entry.free_state_slots.push_back(index - 1U);
            }
            catch (const std::bad_alloc&)
            {
                if (entry.over_aligned)
                {
                    ::operator delete(
                        entry.state_slab,
                        std::align_val_t{alignment}
                    );
                }
                else
                {
                    ::operator delete(entry.state_slab);
                }
                entry.state_slab = nullptr;
                return false;
            }
            return true;
        }

        [[nodiscard]] bool expectedLayout(
            const lux::rdesc::ScriptValueType& semantic,
            lux_script_type_desc& result
        ) const noexcept
        {
            if (const auto* builtin = lux::semantic::builtinLayout(
                    semantic.type_id))
            {
                if (builtin->canonical_name != semantic.canonical_name)
                    return false;
                result = lux_script_type_desc{
                    builtin->canonical_name.data(),
                    builtin->type_id,
                    builtin->size,
                    builtin->alignment,
                    builtin->abi_kind,
                    static_cast<std::uint8_t>(semantic.pass),
                    {}};
                return true;
            }
            const bool is_portable_custom_scalar = semantic.abi_kind >= LUX_SCRIPT_VK_BOOL &&
                semantic.abi_kind <= LUX_SCRIPT_VK_DOUBLE;
            if (is_portable_custom_scalar)
            {
                result = lux_script_type_desc{
                    semantic.canonical_name.c_str(),
                    semantic.type_id,
                    semantic.size,
                    semantic.alignment,
                    semantic.abi_kind,
                    static_cast<std::uint8_t>(semantic.pass),
                    {}};
                return true;
            }
            return record_layouts.resolve && record_layouts.resolve(
                record_layouts.context,
                semantic.type_id,
                semantic.canonical_name,
                result
            );
        }

        [[nodiscard]] bool sameType(
            const lux_script_type_desc& native_type,
            const lux::rdesc::ScriptValueType& semantic
        ) const noexcept
        {
            lux_script_type_desc expected{};
            return expectedLayout(semantic, expected) && native_type.name &&
                native_type.type_id == semantic.type_id &&
                semantic.canonical_name == native_type.name &&
                native_type.kind == expected.kind &&
                native_type.pass == static_cast<std::uint8_t>(semantic.pass) &&
                native_type.size == expected.size &&
                native_type.align == expected.align;
        }

        [[nodiscard]] bool sameType(
            const lux_script_type_desc& native_type,
            const lux::script::ScriptAbilityValueDescription& semantic
        ) const noexcept
        {
            return native_type.name != nullptr && native_type.type_id == semantic.type_id &&
                semantic.canonical_name == native_type.name && native_type.kind == semantic.abi_kind &&
                native_type.pass == static_cast<std::uint8_t>(semantic.pass) && native_type.size == semantic.size &&
                native_type.align == semantic.alignment;
        }

        [[nodiscard]] EScriptBackendResult bindAbilities(
            Instance& instance,
            const ScriptInstanceCreateContext& context
        ) noexcept
        {
            const auto imports = instance.module->module->abilityImports();
            if (imports.size() > config.max_ability_imports_per_module)
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            if (instance.module->module->eventWaitImports().size() > config.max_event_wait_imports_per_module)
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            try
            {
                instance.abilities.clear();
                instance.abilities.reserve(imports.size());
                for (const auto& import : imports)
                {
                    const auto contribution = std::ranges::find_if(ability_contributions, [&](const auto& candidate) {
                        return candidate.description != nullptr &&
                            candidate.description->id.hash() == import.contract_id &&
                            candidate.description->id.name() == import.contract_name;
                    });
                    if (contribution == ability_contributions.end() ||
                        contribution->description->schema_hash != import.schema_hash ||
                        contribution->description->schema_version != import.schema_version)
                    {
                        return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                    }
                    const auto projected = std::ranges::find_if(contribution->methods, [&](const auto& candidate) {
                        return candidate.method.hash() == import.method_id &&
                            candidate.method.name() == import.method_name;
                    });
                    if (projected == contribution->methods.end() || projected->entry == nullptr)
                        return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;

                    const PreparedScriptApiCapability* capability{};
                    for (const auto& candidate : context.capabilities)
                    {
                        if (candidate.contract.hash() == import.contract_id &&
                            candidate.contract.name() == import.contract_name)
                        {
                            capability = std::addressof(candidate);
                            break;
                        }
                    }
                    if (capability == nullptr || capability->schema_hash != import.schema_hash ||
                        capability->schema_version != import.schema_version)
                    {
                        return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                    }

                    const lux::script::ScriptAbilityErasedMethodBinding* method{};
                    for (const auto& candidate : capability->methods)
                    {
                        if (candidate.method.hash() == import.method_id &&
                            candidate.method.name() == import.method_name)
                        {
                            method = std::addressof(candidate);
                            break;
                        }
                    }
                    const bool is_invalid_method = method == nullptr ||
                        static_cast<std::uint8_t>(method->kind) != import.method_kind ||
                        method->parameters.size() != import.arg_count || method->results.size() != import.result_count;
                    if (is_invalid_method)
                        return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                    for (std::size_t index{}; index < method->parameters.size(); ++index)
                    {
                        if (!sameType(import.args[index], method->parameters[index].value))
                            return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                    }
                    for (std::size_t index{}; index < method->results.size(); ++index)
                    {
                        if (!sameType(import.results[index], method->results[index]))
                            return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                    }
                    instance.abilities.push_back({
                        capability->context,
                        capability->dispatch,
                        projected->entry
                    });
                }
            }
            catch (const std::bad_alloc&)
            {
                return EScriptBackendResult::ALLOCATION_FAILURE;
            }
            instance.native_context = {
                instance.state,
                instance.abilities.data(),
                static_cast<std::uint32_t>(instance.abilities.size()),
                0U
            };
            return EScriptBackendResult::SUCCESS;
        }

        [[nodiscard]] EScriptBackendResult bindEvents(
            Instance& instance,
            const ScriptInstanceCreateContext& context
        ) noexcept
        {
            const auto imports = instance.module->module->eventWaitImports();
            if (imports.size() != context.events.size() || imports.size() > config.max_event_wait_imports_per_module)
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            try
            {
                instance.events.clear();
                instance.events.reserve(imports.size());
                for (const auto& import : imports)
                {
                    const auto found = std::ranges::find_if(context.events, [&](const auto& source) noexcept {
                        return source.system_id == import.system_id && source.event_id == import.event_id;
                    });
                    if (found == context.events.end() || !scriptEventImportMatches(import, *found))
                        return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                    instance.events.push_back({std::addressof(*found), std::addressof(import)});
                }
                return EScriptBackendResult::SUCCESS;
            }
            catch (const std::bad_alloc&)
            {
                return EScriptBackendResult::ALLOCATION_FAILURE;
            }
        }

        static int invokePrepared(lux_script_call_frame* frame) noexcept
        {
            if (frame == nullptr || frame->user_context == nullptr)
                return static_cast<std::int32_t>(lux::script::EScriptAbilityErasedCallStatus::INVALID_ARGUMENTS);
            auto& prepared = *static_cast<PreparedCall*>(frame->user_context);
            if (prepared.instance == nullptr || prepared.function == nullptr || prepared.function->invoke == nullptr)
                return static_cast<std::int32_t>(lux::script::EScriptAbilityErasedCallStatus::INVALID_ARGUMENTS);
            const auto* previous_native = frame->native_instance;
            void* previous_user = frame->user_context;
            frame->native_instance = std::addressof(prepared.instance->native_context);
            frame->user_context = prepared.instance->state;
            const auto status = prepared.function->invoke(frame);
            frame->user_context = previous_user;
            frame->native_instance = previous_native;
            return status;
        }

        static int startEventWait(
            void* opaque,
            std::uint32_t ordinal,
            lux_script_async_token* waiting_on
        ) noexcept
        {
            auto& adapter = *static_cast<StepAdapter*>(opaque);
            if (adapter.call == nullptr || adapter.call->instance == nullptr || adapter.context == nullptr ||
                adapter.call->instance->module == nullptr || adapter.call->instance->module->module == nullptr ||
                waiting_on == nullptr)
            {
                return -1;
            }
            if (ordinal >= adapter.call->instance->events.size())
                return -1;
            const auto& prepared = adapter.call->instance->events[ordinal];
            if (prepared.source == nullptr || prepared.import == nullptr)
                return -1;
            const auto& source = *prepared.source;
            const auto route = source.route == lux::script::EScriptEventRoute::SIMULATION_BROADCAST
                ? EEventRoute::SIMULATION_BROADCAST
                : EEventRoute::ENTITY_TARGETED;
            const auto result = adapter.context->event_waits.wait({
                lux::system::SystemInstanceId{source.system_id},
                EventPointId{source.event_id},
                route
            });
            if (!result)
                return -1000 - static_cast<std::int32_t>(result.error());
            if (adapter.continuation != nullptr)
                adapter.continuation->waiting_event_payload = std::addressof(prepared.import->payload);
            waiting_on->slot = result->slot;
            waiting_on->generation = result->generation;
            return 0;
        }

        [[nodiscard]] static ScriptStepResult stepResult(lux_script_step_outcome outcome) noexcept
        {
            switch (outcome.state)
            {
            case LUX_SCRIPT_STEP_COMPLETED:
                return ScriptStepResult::completed();
            case LUX_SCRIPT_STEP_SUSPENDED:
                return ScriptStepResult::suspended({outcome.waiting_on.slot, outcome.waiting_on.generation});
            case LUX_SCRIPT_STEP_FAILED:
                return ScriptStepResult::failed(outcome.status == 0 ? -1 : outcome.status);
            default:
                return ScriptStepResult::failed(-1);
            }
        }

        [[nodiscard]] NativeContinuation* createNativeContinuation(PreparedCall& call) noexcept
        {
            if (free_continuations.empty() || call.function == nullptr || call.function->step == nullptr)
                return nullptr;
            const auto& step = *call.function->step;
            if (step.frame_size == 0U || step.frame_size > config.max_continuation_frame_bytes ||
                step.frame_align == 0U || (step.frame_align & (step.frame_align - 1U)) != 0U)
            {
                return nullptr;
            }
            const auto alignment = static_cast<std::size_t>(step.frame_align);
            auto frame = frame_storage.acquire(step.frame_size, alignment);
            if (!frame)
                return nullptr;
            std::memset(frame->data, 0, step.frame_size);
            const auto slot = free_continuations.back();
            free_continuations.pop_back();
            auto& continuation = continuations[slot];
            continuation = {this, std::addressof(call), *frame, slot};
            return std::addressof(continuation);
        }

        static void destroyNativeContinuation(NativeContinuation& continuation) noexcept
        {
            if (!continuation.frame || continuation.call == nullptr ||
                continuation.call->function == nullptr || continuation.call->function->step == nullptr)
            {
                return;
            }
            continuation.call->function->step->destroy(continuation.frame.data);
            auto* owner = continuation.owner;
            const auto slot = continuation.slot;
            if (owner != nullptr)
                static_cast<void>(owner->frame_storage.release(continuation.frame));
            continuation = {};
            if (owner != nullptr && slot < owner->continuations.size())
                owner->free_continuations.push_back(slot);
        }

        static ScriptStepResult resumeNativeContinuation(
            void* opaque,
            ScriptStepContext& context,
            const ScriptResumePacket& packet
        ) noexcept
        {
            auto& continuation = *static_cast<NativeContinuation*>(opaque);
            if (continuation.call == nullptr || continuation.call->instance == nullptr ||
                continuation.call->function == nullptr || continuation.call->function->step == nullptr)
            {
                return ScriptStepResult::failed(-1);
            }
            if (packet.state == EScriptAwaitableState::READY && continuation.waiting_event_payload != nullptr)
            {
                const auto* actual = packet.value != nullptr && packet.value->type.valid()
                    ? std::addressof(packet.value->type)
                    : nullptr;
                const auto& expected = *continuation.waiting_event_payload;
                const bool is_mismatch = actual == nullptr || actual->type_id != expected.type_id ||
                    actual->abi_kind != expected.kind || actual->size != expected.size ||
                    actual->alignment != expected.align ||
                    packet.value->bytes.size() != expected.size;
                if (is_mismatch)
                    return ScriptStepResult::failed(-1);
            }
            continuation.waiting_event_payload = nullptr;
            lux_script_step_resume_packet native_packet{};
            switch (packet.state)
            {
            case EScriptAwaitableState::READY: native_packet.state = LUX_SCRIPT_RESUME_READY; break;
            case EScriptAwaitableState::FAILED: native_packet.state = LUX_SCRIPT_RESUME_FAILED; break;
            case EScriptAwaitableState::CANCELLED:
            case EScriptAwaitableState::PENDING: native_packet.state = LUX_SCRIPT_RESUME_CANCELLED; break;
            }
            native_packet.status = packet.error.status;
            if (packet.value != nullptr && packet.value->type.valid() && !packet.value->bytes.empty())
            {
                native_packet.has_value = 1U;
                native_packet.value = {
                    packet.value->type.abi_kind,
                    {},
                    static_cast<std::uint32_t>(packet.value->bytes.size()),
                    packet.value->type.type_id,
                    const_cast<std::byte*>(packet.value->bytes.data())
                };
            }
            StepAdapter adapter{continuation.call, std::addressof(context), std::addressof(continuation)};
            const lux_script_step_host host{std::addressof(adapter), &startEventWait};
            lux_script_step_outcome outcome{};
            const auto status = continuation.call->function->step->resume(
                std::addressof(host),
                continuation.frame.data,
                std::addressof(native_packet),
                std::addressof(outcome)
            );
            return status == 0 ? stepResult(outcome) : ScriptStepResult::failed(status);
        }

        static void destroyNativeContinuationErased(void* opaque) noexcept
        {
            if (opaque != nullptr)
                destroyNativeContinuation(*static_cast<NativeContinuation*>(opaque));
        }

        static ScriptStepResult invokePreparedStep(
            void* opaque,
            lux_script_call_frame& frame,
            ScriptStepContext& context,
            ScriptBackendContinuation& result
        ) noexcept
        {
            auto& prepared = *static_cast<PreparedCall*>(opaque);
            if (prepared.owner == nullptr || prepared.instance == nullptr || prepared.function == nullptr ||
                prepared.function->step == nullptr)
                return ScriptStepResult::failed(-1);
            auto* continuation = prepared.owner->createNativeContinuation(prepared);
            if (continuation == nullptr)
                return ScriptStepResult::failed(-1);

            const auto* previous_native = frame.native_instance;
            void* previous_user = frame.user_context;
            frame.native_instance = std::addressof(prepared.instance->native_context);
            frame.user_context = prepared.instance->state;
            StepAdapter adapter{std::addressof(prepared), std::addressof(context), continuation};
            const lux_script_step_host host{std::addressof(adapter), &startEventWait};
            lux_script_step_outcome outcome{};
            const auto status = prepared.function->step->start(
                std::addressof(frame),
                std::addressof(host),
                continuation->frame.data,
                std::addressof(outcome)
            );
            frame.user_context = previous_user;
            frame.native_instance = previous_native;
            if (status != 0)
            {
                destroyNativeContinuation(*continuation);
                return ScriptStepResult::failed(status);
            }
            const auto step_result = stepResult(outcome);
            if (step_result.state != EScriptStepState::SUSPENDED || !step_result.valid())
            {
                destroyNativeContinuation(*continuation);
                return step_result.valid() ? step_result : ScriptStepResult::failed(-1);
            }
            result = {
                continuation,
                &resumeNativeContinuation,
                &destroyNativeContinuationErased
            };
            return step_result;
        }

        [[nodiscard]] bool executableContractMatches(
            const lux::script::NativeModule& module,
            const lux::script::ScriptArtifact& artifact
        ) const noexcept
        {
            const auto* body = std::get_if<lux::rdesc::NativeModuleScript>(
                std::addressof(artifact.description().body)
            );
            if (!body || body->abi_version != LUX_SCRIPT_ABI_VERSION ||
                module.abiVersion() != LUX_SCRIPT_ABI_VERSION ||
                module.name() != artifact.description().module_name ||
                module.stateLayoutHash() != body->state_layout_hash ||
                module.stateSize() != body->state_size ||
                module.stateAlignment() != body->state_align)
            {
                return false;
            }
            const auto functions = module.functions();
            if (functions.size() != artifact.description().exports.size())
                return false;
            for (std::size_t function_index{};
                 function_index < functions.size(); ++function_index)
            {
                const auto& native = functions[function_index];
                const auto& semantic = artifact.description().exports[function_index];
                if (!native.name || semantic.name != native.name ||
                    semantic.symbol_id != native.symbol_id ||
                    semantic.args.size() != native.arg_count ||
                    semantic.returns.size() != native.return_count)
                {
                    return false;
                }
                for (std::size_t index{}; index < semantic.args.size(); ++index)
                {
                    if (!sameType(native.args[index], semantic.args[index]))
                        return false;
                }
                for (std::size_t index{};
                     index < semantic.returns.size(); ++index)
                {
                    if (!sameType(native.returns[index], semantic.returns[index]))
                        return false;
                }
            }
            const auto event_imports = module.eventWaitImports();
            if (event_imports.size() != artifact.description().event_requirements.size())
                return false;
            for (const auto& import : event_imports)
            {
                const auto found = std::ranges::find_if(
                    artifact.description().event_requirements,
                    [&](const auto& source) noexcept {
                        return source.system_id == import.system_id && source.event_id == import.event_id;
                    }
                );
                if (found == artifact.description().event_requirements.end() ||
                    !scriptEventImportMatches(import, *found))
                    return false;
            }
            return true;
        }

        [[nodiscard]] EScriptBackendResult resolveModule(
            const lux::asset::AssetId& asset_id,
            const lux::script::ScriptArtifact& artifact,
            ModuleEntry*& result
        ) noexcept
        {
            const auto found = module_index.find(asset_id);
            if (found != module_index.end())
            {
                auto& entry = modules[found->second];
                if (!executableContractMatches(*entry.module, artifact))
                {
                    return EScriptBackendResult::
                        EXECUTABLE_CONTRACT_MISMATCH;
                }
                result = std::addressof(entry);
                return EScriptBackendResult::SUCCESS;
            }
            if (modules.size() >= module_capacity)
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            ResolvedNativeModule resolved;
            if (!resolver.resolve ||
                !resolver.resolve(
                    resolver.context,
                    asset_id,
                    artifact,
                    resolved
                ) ||
                !resolved.module)
            {
                if (resolved.release)
                    resolved.release(resolved.lease);
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            }
            if (!executableContractMatches(*resolved.module, artifact))
            {
                if (resolved.release)
                    resolved.release(resolved.lease);
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            }
            bool module_appended{};
            try
            {
                modules.push_back(ModuleEntry{
                    asset_id,
                    resolved.module,
                    resolved.lease,
                    resolved.release});
                module_appended = true;
                auto& entry = modules.back();
                const auto& body = std::get<lux::rdesc::NativeModuleScript>(
                    artifact.description().body
                );
                if (!initializeStateSlab(entry, body))
                {
                    modules.pop_back();
                    if (resolved.release)
                        resolved.release(resolved.lease);
                    return EScriptBackendResult::ALLOCATION_FAILURE;
                }
                const auto module_slot = modules.size() - 1U;
                if (!module_index.emplace(asset_id, module_slot).second)
                {
                    if (entry.state_slab)
                    {
                        if (entry.over_aligned)
                        {
                            ::operator delete(
                                entry.state_slab,
                                std::align_val_t{entry.state_align}
                            );
                        }
                        else
                        {
                            ::operator delete(entry.state_slab);
                        }
                    }
                    modules.pop_back();
                    if (resolved.release)
                        resolved.release(resolved.lease);
                    return EScriptBackendResult::CONSTRUCTION_FAILURE;
                }
                resolved.lease = nullptr;
                resolved.release = nullptr;
                result = std::addressof(entry);
                return EScriptBackendResult::SUCCESS;
            }
            catch (const std::bad_alloc&)
            {
                if (module_appended)
                {
                    auto& entry = modules.back();
                    if (entry.state_slab)
                    {
                        if (entry.over_aligned)
                        {
                            ::operator delete(
                                entry.state_slab,
                                std::align_val_t{entry.state_align}
                            );
                        }
                        else
                        {
                            ::operator delete(entry.state_slab);
                        }
                    }
                    modules.pop_back();
                }
                if (resolved.release)
                    resolved.release(resolved.lease);
                return EScriptBackendResult::ALLOCATION_FAILURE;
            }
        }

        static EScriptBackendResult createInstance(
            void* opaque,
            const ScriptInstanceCreateContext& context,
            const lux::script::ScriptArtifact& artifact,
            ScriptBackendInstance& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            const auto* body = std::get_if<lux::rdesc::NativeModuleScript>(
                std::addressof(artifact.description().body));
            if (!body || body->state_align == 0U ||
                (body->state_align & (body->state_align - 1U)) != 0U ||
                body->state_defaults.size() > body->state_size)
            {
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            }
            if (self.free_instances.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            ModuleEntry* module{};
            const auto module_result = self.resolveModule(
                context.asset,
                artifact,
                module
            );
            if (module_result != EScriptBackendResult::SUCCESS)
                return module_result;
            if (body->state_size != 0U && module->free_state_slots.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            const auto instance_slot = self.free_instances.back();
            self.free_instances.pop_back();
            auto* instance = std::addressof(self.instances[instance_slot]);
            *instance = {};
            instance->module = module;
            instance->state_size = body->state_size;
            instance->state_align = body->state_align;
            instance->over_aligned = body->state_align >
                __STDCPP_DEFAULT_NEW_ALIGNMENT__;
            if (body->state_size != 0U)
            {
                instance->state_slot = module->free_state_slots.back();
                module->free_state_slots.pop_back();
                instance->state = static_cast<std::byte*>(module->state_slab) +
                    instance->state_slot * module->state_stride;
                std::memset(instance->state, 0, body->state_size);
                if (!body->state_defaults.empty())
                {
                    std::memcpy(
                        instance->state,
                        body->state_defaults.data(),
                        body->state_defaults.size()
                    );
                }
            }
            const auto abilities = self.bindAbilities(*instance, context);
            if (abilities != EScriptBackendResult::SUCCESS)
            {
                if (instance->state)
                    module->free_state_slots.push_back(instance->state_slot);
                *instance = {};
                self.free_instances.push_back(instance_slot);
                return abilities;
            }
            const auto events = self.bindEvents(*instance, context);
            if (events != EScriptBackendResult::SUCCESS)
            {
                if (instance->state)
                    module->free_state_slots.push_back(instance->state_slot);
                *instance = {};
                self.free_instances.push_back(instance_slot);
                return events;
            }
            ++self.live_instances;
            result.value = instance;
            return EScriptBackendResult::SUCCESS;
        }

        static EScriptBackendResult prepareMethod(
            void* opaque,
            ScriptBackendInstance instance_value,
            const lux::rdesc::ScriptFunction& description,
            ScriptBackendPreparedMethod& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* instance = static_cast<Instance*>(instance_value.value);
            if (!instance || !instance->module || !instance->module->module)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            const auto* function = instance->module->module->findFunction(
                description.symbol_id);
            if (!function || !function->invoke ||
                function->arg_count != description.args.size() ||
                function->return_count != description.returns.size())
            {
                return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
            }
            for (std::size_t index{}; index < description.args.size(); ++index)
            {
                if (!self.sameType(function->args[index], description.args[index]))
                    return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
            }
            for (std::size_t index{}; index < description.returns.size(); ++index)
            {
                if (!self.sameType(
                        function->returns[index],
                        description.returns[index]))
                {
                    return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
                }
            }
            if (self.free_prepared_calls.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            const auto slot = self.free_prepared_calls.back();
            self.free_prepared_calls.pop_back();
            auto& prepared = self.prepared_calls[slot];
            prepared = {std::addressof(self), instance, function};
            result = {
                std::addressof(prepared),
                lux::script::BoundScriptCall{&invokePrepared, std::addressof(prepared)},
                function->step == nullptr
                    ? BoundScriptStepCall{}
                    : BoundScriptStepCall{std::addressof(prepared), &invokePreparedStep}
            };
            return EScriptBackendResult::SUCCESS;
        }

        static void releaseMethod(
            void* opaque,
            ScriptBackendInstance,
            ScriptBackendPreparedMethod method
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* prepared = static_cast<PreparedCall*>(method.token);
            const auto address = reinterpret_cast<std::uintptr_t>(prepared);
            const auto begin = reinterpret_cast<std::uintptr_t>(self.prepared_calls.data());
            const auto end = begin + self.prepared_calls.size() * sizeof(PreparedCall);
            if (prepared == nullptr || address < begin || address >= end ||
                (address - begin) % sizeof(PreparedCall) != 0U)
            {
                return;
            }
            const auto slot = static_cast<std::size_t>(prepared - self.prepared_calls.data());
            if (prepared->instance == nullptr)
                return;
            *prepared = {};
            self.free_prepared_calls.push_back(slot);
        }

        static void destroyInstance(
            void* opaque,
            ScriptBackendInstance instance_value
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* instance = static_cast<Instance*>(instance_value.value);
            if (!instance)
                return;
            instance->abilities.clear();
            if (instance->state)
            {
                std::memset(instance->state, 0, instance->state_size);
                instance->module->free_state_slots.push_back(
                    instance->state_slot
                );
            }
            const auto instance_slot = static_cast<std::size_t>(
                instance - self.instances.data()
            );
            *instance = {};
            self.free_instances.push_back(instance_slot);
            if (self.live_instances != 0U)
                --self.live_instances;
        }

        NativeModuleResolver resolver;
        NativeScriptBackendConfig config;
        std::size_t module_capacity{};
        std::size_t instance_capacity{};
        std::size_t live_instances{};
        NativeScriptRecordLayoutResolver record_layouts;
        std::vector<lux::script::native::ScriptAbilityNativeContribution> ability_contributions;
        std::vector<ModuleEntry> modules;
        std::unordered_map<lux::asset::AssetId, std::size_t> module_index;
        std::vector<Instance> instances;
        std::vector<std::size_t> free_instances;
        std::vector<PreparedCall> prepared_calls;
        std::vector<std::size_t> free_prepared_calls;
        std::vector<NativeContinuation> continuations;
        std::vector<std::size_t> free_continuations;
        detail::BoundedFrameStorage frame_storage;
    };

    ScriptStepContext* detail::NativeAbilityProjectionAccess::step(void* invocation) noexcept
    {
        if (invocation == nullptr)
            return nullptr;
        auto& adapter = *static_cast<NativeScriptBackend::State::StepAdapter*>(invocation);
        return adapter.context;
    }

    void detail::NativeAbilityProjectionAccess::beginAbility(void* invocation) noexcept
    {
        if (invocation == nullptr)
            return;
        auto& adapter = *static_cast<NativeScriptBackend::State::StepAdapter*>(invocation);
        if (adapter.continuation != nullptr)
            adapter.continuation->waiting_event_payload = nullptr;
    }

    NativeScriptBackend::NativeScriptBackend(
        NativeModuleResolver resolver,
        NativeScriptBackendConfig config
    ) noexcept
    {
        const bool is_invalid_config = config.module_capacity == 0U || config.instance_capacity == 0U ||
            config.prepared_call_capacity == 0U || config.continuation_capacity == 0U ||
            config.max_ability_imports_per_module == 0U || config.max_continuation_frame_bytes == 0U ||
            config.continuation_frame_storage_bytes < config.max_continuation_frame_bytes ||
            config.continuation_frame_storage_alignment < alignof(std::max_align_t) ||
            (config.continuation_frame_storage_alignment & (config.continuation_frame_storage_alignment - 1U)) !=
                0U ||
            config.max_event_wait_imports_per_module == 0U;
        if (!resolver.resolve || is_invalid_config)
        {
            return;
        }
        for (std::size_t index{}; index < config.abilities.size(); ++index)
        {
            if (!config.abilities[index].valid())
                return;
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (config.abilities[previous].description->id == config.abilities[index].description->id)
                    return;
            }
        }
        try
        {
            auto frame_storage = detail::BoundedFrameStorage::create(
                config.continuation_frame_storage_bytes,
                config.continuation_capacity,
                config.continuation_frame_storage_alignment
            );
            if (!frame_storage)
                return;
            state_ = std::make_unique<State>(
                resolver,
                config,
                std::move(*frame_storage)
            );
        }
        catch (const std::bad_alloc&)
        {
        }
    }

    NativeScriptBackend::~NativeScriptBackend() = default;
    NativeScriptBackend::NativeScriptBackend(
        NativeScriptBackend&&
    ) noexcept = default;
    NativeScriptBackend& NativeScriptBackend::operator=(
        NativeScriptBackend&&
    ) noexcept = default;

    NativeScriptBackend::operator bool() const noexcept
    {
        return state_ != nullptr;
    }

    NativeScriptBackendStats NativeScriptBackend::stats() const noexcept
    {
        if (!state_)
            return {};
        const auto stats = state_->frame_storage.stats();
        return {
            stats.storage_bytes,
            stats.active_frames,
            stats.frame_high_water,
            stats.capacity_failures,
            0U
        };
    }

    ScriptBackendDescriptor NativeScriptBackend::descriptor() noexcept
    {
        return state_
            ? ScriptBackendDescriptor{
                lux::rdesc::Script::Kind::NATIVE_MODULE,
                state_.get(),
                &State::createInstance,
                &State::prepareMethod,
                &State::releaseMethod,
                &State::destroyInstance}
            : ScriptBackendDescriptor{};
    }
}
