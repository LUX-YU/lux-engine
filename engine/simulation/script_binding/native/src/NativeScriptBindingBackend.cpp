#include <lux/engine/simulation/NativeScriptBindingBackend.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>

namespace lux::simulation
{
    struct NativeScriptBindingBackend::State final
    {
        struct Instance final
        {
            void* state{};
            std::size_t state_size{};
            std::size_t state_align{1U};
        };

        State(
            std::shared_ptr<lux::script::NativeModule> value,
            std::size_t capacity,
            NativeScriptRecordLayoutResolver layouts
        ) noexcept
            : module(std::move(value)),
              instance_capacity(capacity),
              record_layouts(layouts)
        {
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
                native_type.size == expected.size &&
                native_type.align == expected.align;
        }

        static EScriptBackendResult createInstance(
            void* opaque,
            const ScriptInstanceCreateContext&,
            const lux::asset::ScriptAssetContent& asset,
            ScriptBackendInstance& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            const auto* body = std::get_if<lux::rdesc::NativeModuleScript>(
                std::addressof(asset.description.body));
            if (!self.module || !body ||
                body->abi_version != self.module->abiVersion() ||
                body->state_align == 0U ||
                (body->state_align & (body->state_align - 1U)) != 0U ||
                body->state_defaults.size() > body->state_size)
            {
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            }
            if (self.live_instances >= self.instance_capacity)
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            auto* instance = new (std::nothrow) Instance;
            if (!instance)
                return EScriptBackendResult::ALLOCATION_FAILURE;
            instance->state_size = body->state_size;
            instance->state_align = body->state_align;
            if (body->state_size != 0U)
            {
                instance->state = ::operator new(
                    body->state_size,
                    std::align_val_t{body->state_align},
                    std::nothrow
                );
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
            if (!instance || !self.module)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            const auto* function = self.module->findFunction(
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
                ::operator delete(
                    instance->state,
                    std::align_val_t{instance->state_align}
                );
            }
            delete instance;
            if (self.live_instances != 0U)
                --self.live_instances;
        }

        std::shared_ptr<lux::script::NativeModule> module;
        std::size_t instance_capacity{};
        std::size_t live_instances{};
        NativeScriptRecordLayoutResolver record_layouts;
    };

    NativeScriptBindingBackend::NativeScriptBindingBackend(
        std::shared_ptr<lux::script::NativeModule> module,
        std::size_t instance_capacity,
        NativeScriptRecordLayoutResolver record_layouts
    ) noexcept
    {
        try
        {
            state_ = std::make_unique<State>(
                std::move(module),
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
        return state_ && state_->module;
    }

    ScriptBackendDescriptor NativeScriptBindingBackend::descriptor() noexcept
    {
        return ScriptBackendDescriptor{
            lux::rdesc::Script::Kind::NATIVE_MODULE,
            state_.get(),
            &State::createInstance,
            &State::prepareMethod,
            &State::releaseMethod,
            &State::destroyInstance};
    }
}
