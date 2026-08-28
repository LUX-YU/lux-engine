#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>

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
            ModuleEntry* module{};
            void* state{};
            std::size_t state_size{};
            std::size_t state_align{1U};
            bool over_aligned{};
            std::size_t state_slot{(std::numeric_limits<std::size_t>::max)()};
        };

        State(
            NativeModuleResolver source_resolver,
            std::size_t source_module_capacity,
            std::size_t source_instance_capacity,
            NativeScriptRecordLayoutResolver layouts
        )
            : resolver(source_resolver),
              module_capacity(source_module_capacity),
              instance_capacity(source_instance_capacity),
              record_layouts(layouts)
        {
            modules.reserve(module_capacity);
            module_index.reserve(module_capacity);
            instances.resize(instance_capacity);
            free_instances.reserve(instance_capacity);
            for (std::size_t index = instance_capacity; index > 0U; --index)
                free_instances.push_back(index - 1U);
        }

        ~State()
        {
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
            if (const auto* builtin = lux::script::scriptBuiltinLayout(
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
            ++self.live_instances;
            result.value = instance;
            return EScriptBackendResult::SUCCESS;
        }

        static EScriptBackendResult prepareMethod(
            void* opaque,
            ScriptBackendInstance instance_value,
            const lux::rdesc::ScriptFunction& description,
            lux::script::BoundScriptCall& result
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
            result = lux::script::BoundScriptCall{
                function->invoke,
                instance->state};
            return EScriptBackendResult::SUCCESS;
        }

        static void releaseMethod(
            void*,
            ScriptBackendInstance,
            lux::script::BoundScriptCall
        ) noexcept
        {
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
        std::size_t module_capacity{};
        std::size_t instance_capacity{};
        std::size_t live_instances{};
        NativeScriptRecordLayoutResolver record_layouts;
        std::vector<ModuleEntry> modules;
        std::unordered_map<lux::asset::AssetId, std::size_t> module_index;
        std::vector<Instance> instances;
        std::vector<std::size_t> free_instances;
    };

    NativeScriptBackend::NativeScriptBackend(
        NativeModuleResolver resolver,
        std::size_t module_capacity,
        std::size_t instance_capacity,
        NativeScriptRecordLayoutResolver record_layouts
    ) noexcept
    {
        if (!resolver.resolve || module_capacity == 0U ||
            instance_capacity == 0U)
        {
            return;
        }
        try
        {
            state_ = std::make_unique<State>(
                resolver,
                module_capacity,
                instance_capacity,
                record_layouts
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
