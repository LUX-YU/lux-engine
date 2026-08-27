#include <lux/engine/simulation/NativeScriptBindingBackend.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace lux::simulation
{
    struct NativeScriptBindingBackend::State final
    {
        struct ModuleEntry final
        {
            lux::asset::AssetId asset;
            const lux::script::NativeModule* module{};
            void* lease{};
            void (*release)(void*) noexcept{};
        };

        struct Instance final
        {
            ModuleEntry* module{};
            void* state{};
            std::size_t state_size{};
            std::size_t state_align{1U};
            bool over_aligned{};
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
        }

        ~State()
        {
            for (auto entry = modules.rbegin(); entry != modules.rend(); ++entry)
            {
                if (entry->release)
                    entry->release(entry->lease);
            }
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
            const lux::asset::ScriptAssetContent& asset
        ) const noexcept
        {
            const auto* body = std::get_if<lux::rdesc::NativeModuleScript>(
                std::addressof(asset.description.body)
            );
            if (!body || body->abi_version != LUX_SCRIPT_ABI_VERSION ||
                module.abiVersion() != LUX_SCRIPT_ABI_VERSION ||
                module.name() != asset.description.module_name ||
                module.stateLayoutHash() != body->state_layout_hash ||
                module.stateSize() != body->state_size ||
                module.stateAlignment() != body->state_align)
            {
                return false;
            }
            const auto functions = module.functions();
            if (functions.size() != asset.description.exports.size())
                return false;
            for (std::size_t function_index{};
                 function_index < functions.size(); ++function_index)
            {
                const auto& native = functions[function_index];
                const auto& semantic = asset.description.exports[function_index];
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
            const lux::asset::ScriptAssetContent& asset,
            ModuleEntry*& result
        ) noexcept
        {
            const auto found = std::find_if(
                modules.begin(),
                modules.end(),
                [&](const ModuleEntry& entry) noexcept
                {
                    return entry.asset == asset_id;
                }
            );
            if (found != modules.end())
            {
                if (!executableContractMatches(*found->module, asset))
                {
                    return EScriptBackendResult::
                        EXECUTABLE_CONTRACT_MISMATCH;
                }
                result = std::addressof(*found);
                return EScriptBackendResult::SUCCESS;
            }
            if (modules.size() >= module_capacity)
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            ResolvedNativeModule resolved;
            if (!resolver.resolve ||
                !resolver.resolve(
                    resolver.context,
                    asset_id,
                    asset,
                    resolved
                ) ||
                !resolved.module)
            {
                if (resolved.release)
                    resolved.release(resolved.lease);
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            }
            if (!executableContractMatches(*resolved.module, asset))
            {
                if (resolved.release)
                    resolved.release(resolved.lease);
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            }
            try
            {
                modules.push_back(ModuleEntry{
                    asset_id,
                    resolved.module,
                    resolved.lease,
                    resolved.release});
                resolved.lease = nullptr;
                resolved.release = nullptr;
                result = std::addressof(modules.back());
                return EScriptBackendResult::SUCCESS;
            }
            catch (const std::bad_alloc&)
            {
                if (resolved.release)
                    resolved.release(resolved.lease);
                return EScriptBackendResult::ALLOCATION_FAILURE;
            }
        }

        static EScriptBackendResult createInstance(
            void* opaque,
            const ScriptInstanceCreateContext& context,
            const lux::asset::ScriptAssetContent& asset,
            ScriptBackendInstance& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            const auto* body = std::get_if<lux::rdesc::NativeModuleScript>(
                std::addressof(asset.description.body));
            if (!body || body->state_align == 0U ||
                (body->state_align & (body->state_align - 1U)) != 0U ||
                body->state_defaults.size() > body->state_size)
            {
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            }
            if (self.live_instances >= self.instance_capacity)
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            ModuleEntry* module{};
            const auto module_result = self.resolveModule(
                context.script,
                asset,
                module
            );
            if (module_result != EScriptBackendResult::SUCCESS)
                return module_result;
            auto* instance = new (std::nothrow) Instance;
            if (!instance)
                return EScriptBackendResult::ALLOCATION_FAILURE;
            instance->module = module;
            instance->state_size = body->state_size;
            instance->state_align = body->state_align;
            instance->over_aligned = body->state_align >
                __STDCPP_DEFAULT_NEW_ALIGNMENT__;
            if (body->state_size != 0U)
            {
                instance->state = instance->over_aligned
                    ? ::operator new(
                        body->state_size,
                        std::align_val_t{body->state_align},
                        std::nothrow)
                    : ::operator new(body->state_size, std::nothrow);
                if (!instance->state)
                {
                    delete instance;
                    return EScriptBackendResult::ALLOCATION_FAILURE;
                }
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
                if (instance->over_aligned)
                {
                    ::operator delete(
                        instance->state,
                        std::align_val_t{instance->state_align}
                    );
                }
                else
                {
                    ::operator delete(instance->state);
                }
            }
            delete instance;
            if (self.live_instances != 0U)
                --self.live_instances;
        }

        NativeModuleResolver resolver;
        std::size_t module_capacity{};
        std::size_t instance_capacity{};
        std::size_t live_instances{};
        NativeScriptRecordLayoutResolver record_layouts;
        std::vector<ModuleEntry> modules;
    };

    NativeScriptBindingBackend::NativeScriptBindingBackend(
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

    NativeScriptBindingBackend::~NativeScriptBindingBackend() = default;
    NativeScriptBindingBackend::NativeScriptBindingBackend(
        NativeScriptBindingBackend&&
    ) noexcept = default;
    NativeScriptBindingBackend& NativeScriptBindingBackend::operator=(
        NativeScriptBindingBackend&&
    ) noexcept = default;

    NativeScriptBindingBackend::operator bool() const noexcept
    {
        return state_ != nullptr;
    }

    ScriptBackendDescriptor NativeScriptBindingBackend::descriptor() noexcept
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
