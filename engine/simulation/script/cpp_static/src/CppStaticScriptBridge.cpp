#include <lux/engine/simulation/script/cpp_static/CppStaticScriptBridge.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace lux::simulation::script
{
    namespace
    {
        struct Callable final
        {
            lux::script::ScriptSymbolId symbol{};
            const lux::meta::RefMethod* method{};
            const lux::meta::RefFunction* function{};
        };

        [[nodiscard]] const lux::script::ScriptSemanticLayout* builtin(
            const lux::meta::RefType& type,
            std::uint64_t value_type_hash
        ) noexcept
        {
            using lux::script::ScriptBuiltinSemanticLayouts;
            struct Mapping final
            {
                lux::meta::EBaseType base;
                std::uint64_t type_hash{};
                lux::script::ScriptSemanticLayout layout;
            };
#define LUX_META_BASE_BOOL Bool
#define LUX_META_BASE_I32 Int32
#define LUX_META_BASE_U32 Uint32
#define LUX_META_BASE_I64 Int64
#define LUX_META_BASE_U64 Uint64
#define LUX_META_BASE_F32 Float
#define LUX_META_BASE_F64 Double
#define LUX_META_BASE_VALUE(tag) LUX_META_BASE_##tag
#define LUX_SEMANTIC_BUILTIN(tag, cpp_type, canonical, abi_kind_value)         \
            Mapping{                                                          \
                lux::meta::EBaseType::LUX_META_BASE_VALUE(tag),                \
                lux::cxx::type_hash<cpp_type>(),                               \
                lux::script::ScriptSemanticLayout{                             \
                    lux::script::scriptSemanticTypeId(canonical),              \
                    canonical,                                                 \
                    abi_kind_value,                                            \
                    sizeof(cpp_type),                                          \
                    alignof(cpp_type)}},
            static const auto mappings = std::array{
#include <lux/engine/core/semantic/SemanticBuiltin.def>
            };
#undef LUX_SEMANTIC_BUILTIN
#undef LUX_META_BASE_VALUE
#undef LUX_META_BASE_F64
#undef LUX_META_BASE_F32
#undef LUX_META_BASE_U64
#undef LUX_META_BASE_I64
#undef LUX_META_BASE_U32
#undef LUX_META_BASE_I32
#undef LUX_META_BASE_BOOL
            const auto found = std::find_if(
                mappings.begin(),
                mappings.end(),
                [&type, value_type_hash](const Mapping& mapping) noexcept
                {
                    return mapping.base == static_cast<lux::meta::EBaseType>(
                        type.qtype.base
                    ) && mapping.type_hash == value_type_hash;
                }
            );
            return found == mappings.end() ? nullptr : std::addressof(found->layout);
        }

        [[nodiscard]] lux::cxx::expected<
            lux::rdesc::ScriptValueType,
            ECppStaticScriptBridgeError> projectType(
                const lux::meta::RefType& type,
                std::uint64_t value_type_hash,
                bool is_return,
                CppStaticRecordSemanticResolver resolver
            ) noexcept
        {
            const auto qualifier = static_cast<lux::meta::ETypeQual>(
                type.qtype.qual
            );
            auto pass = lux::script::EScriptPassMode::VALUE;
            switch (qualifier)
            {
            case lux::meta::ETypeQual::Value:
                break;
            case lux::meta::ETypeQual::LRefToConst:
                if (is_return)
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::RETURN_NOT_SUPPORTED
                    );
                }
                pass = lux::script::EScriptPassMode::CONST_REF;
                break;
            case lux::meta::ETypeQual::LRef:
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::MUTABLE_REFERENCE_NOT_SUPPORTED
                );
            case lux::meta::ETypeQual::RRef:
            case lux::meta::ETypeQual::RRefToConst:
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::RVALUE_REFERENCE_NOT_SUPPORTED
                );
            default:
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::POINTER_NOT_SUPPORTED
                );
            }

            if (const auto* layout = builtin(type, value_type_hash))
            {
                return lux::rdesc::ScriptValueType{
                    std::string{layout->canonical_name},
                    layout->type_id,
                    pass,
                    layout->abi_kind,
                    layout->size,
                    layout->alignment};
            }
            // Non-builtin records and enums have no portable Script identity
            // until the caller explicitly supplies one.  Lifecycle stop
            // reasons are an enum, so restricting this seam to Record would
            // make the canonical STOP signature impossible to project.
            if (!resolver.resolve)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::UNSUPPORTED_TYPE
                );
            }
            lux::script::ScriptSemanticLayout layout;
            if (!resolver.resolve(resolver.context, type, layout) ||
                layout.type_id == 0U || layout.canonical_name.empty() ||
                layout.type_id != lux::script::scriptSemanticTypeId(
                    layout.canonical_name) ||
                layout.size != type.size || layout.alignment != type.alignment)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::UNSUPPORTED_TYPE
                );
            }
            return lux::rdesc::ScriptValueType{
                std::string{layout.canonical_name},
                layout.type_id,
                pass,
                layout.abi_kind,
                layout.size,
                layout.alignment};
        }

        [[nodiscard]] lux::cxx::expected<
            lux::rdesc::ScriptFunction,
            ECppStaticScriptBridgeError> projectInvokable(
                const lux::meta::RefInvokable& invokable,
                lux::script::ScriptSymbolId assigned_symbol,
                bool is_noexcept,
                CppStaticRecordSemanticResolver resolver
            ) noexcept
        {
            if (assigned_symbol == lux::script::InvalidScriptSymbolId)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::INVALID_DESCRIPTOR
                );
            }
            if (!is_noexcept)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::METHOD_NOT_NOEXCEPT
                );
            }
            if (!invokable.invoker)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::MISSING_INVOKER
                );
            }
            if (invokable.is_variadic)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::VARIADIC_NOT_SUPPORTED
                );
            }
            try
            {
                lux::rdesc::ScriptFunction projected;
                projected.name = invokable.name;
                projected.args.reserve(invokable.parameters.size());
                for (const auto& parameter : invokable.parameters)
                {
                    auto type = projectType(
                        parameter.type,
                        parameter.value_type_hash,
                        false,
                        resolver
                    );
                    if (!type)
                        return lux::cxx::unexpected(type.error());
                    projected.args.push_back(std::move(*type));
                }
                const auto return_base = static_cast<lux::meta::EBaseType>(
                    invokable.return_type.qtype.base
                );
                if (return_base != lux::meta::EBaseType::Void)
                {
                    auto type = projectType(
                        invokable.return_type,
                        invokable.return_type.hash,
                        true,
                        resolver
                    );
                    if (!type)
                        return lux::cxx::unexpected(type.error());
                    projected.returns.push_back(std::move(*type));
                }
                projected.symbol_id = assigned_symbol;
                return projected;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::ALLOCATION_FAILURE
                );
            }
        }
    }

    struct CppStaticScriptDescriptor::State final
    {
        lux::rdesc::Script description;
        std::string descriptor_key;
        const lux::meta::RefClass* reflected_class{};
        void (*attach)(void*, ScriptBehavior&) noexcept{};
        bool entity_scope{};
        std::vector<Callable> callables;
    };

    CppStaticScriptDescriptor::CppStaticScriptDescriptor() noexcept = default;
    CppStaticScriptDescriptor::CppStaticScriptDescriptor(
        std::unique_ptr<State> state
    ) noexcept
        : state_(std::move(state))
    {
    }
    CppStaticScriptDescriptor::CppStaticScriptDescriptor(
        CppStaticScriptDescriptor&&
    ) noexcept = default;
    CppStaticScriptDescriptor& CppStaticScriptDescriptor::operator=(
        CppStaticScriptDescriptor&&
    ) noexcept = default;
    CppStaticScriptDescriptor::~CppStaticScriptDescriptor() = default;

    CppStaticScriptDescriptor::operator bool() const noexcept
    {
        return state_ != nullptr;
    }

    const lux::rdesc::Script& CppStaticScriptDescriptor::description() const
        noexcept
    {
        static const lux::rdesc::Script empty;
        return state_ ? state_->description : empty;
    }

    std::string_view CppStaticScriptDescriptor::key() const noexcept
    {
        return state_ ? std::string_view{state_->descriptor_key}
                      : std::string_view{};
    }

    lux::cxx::expected<
        CppStaticScriptDescriptor,
        ECppStaticScriptBridgeError> projectCppStaticEntityScript(
            std::string_view module_name,
            std::string_view descriptor_key,
            const lux::meta::RefClass& reflected_class,
            std::span<const lux::meta::RefMethod* const> methods,
            std::span<const lux::script::ScriptSymbolId> symbols,
            CppStaticRecordSemanticResolver record_types,
            void (*attach)(void*, ScriptBehavior&) noexcept
        ) noexcept
    {
        if (module_name.empty() || descriptor_key.empty() ||
            methods.size() != symbols.size() ||
            !reflected_class.construct || !reflected_class.destruct ||
            reflected_class.type.size == 0U ||
            reflected_class.type.alignment == 0U)
        {
            return lux::cxx::unexpected(
                ECppStaticScriptBridgeError::INVALID_DESCRIPTOR
            );
        }
        try
        {
            auto state = std::make_unique<CppStaticScriptDescriptor::State>();
            state->description.module_name = module_name;
            state->description.body = lux::rdesc::CppStaticScript{
                std::string{descriptor_key}};
            state->descriptor_key = descriptor_key;
            state->reflected_class = std::addressof(reflected_class);
            state->attach = attach;
            state->entity_scope = true;
            state->description.exports.reserve(methods.size());
            state->callables.reserve(methods.size());
            for (std::size_t method_index{}; method_index < methods.size();
                 ++method_index)
            {
                const auto* method = methods[method_index];
                if (!method || method->owner_class != &reflected_class ||
                    method->is_static)
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::INVALID_CLASS
                    );
                }
                if (method->visibility != lux::meta::EVisibility::Public)
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::METHOD_NOT_PUBLIC
                    );
                }
                auto projected = projectInvokable(
                    method->invokable,
                    symbols[method_index],
                    method->is_noexcept,
                    record_types
                );
                if (!projected)
                    return lux::cxx::unexpected(projected.error());
                const auto symbol = projected->symbol_id;
                if (std::any_of(
                        state->callables.begin(),
                        state->callables.end(),
                        [symbol](const auto& value) noexcept
                        {
                            return value.symbol == symbol;
                        }))
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::DUPLICATE_SYMBOL
                    );
                }
                state->description.exports.push_back(std::move(*projected));
                state->callables.push_back(Callable{symbol, method, nullptr});
            }
            return CppStaticScriptDescriptor{std::move(state)};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                ECppStaticScriptBridgeError::ALLOCATION_FAILURE
            );
        }
    }

    lux::cxx::expected<
        CppStaticScriptDescriptor,
        ECppStaticScriptBridgeError> projectCppStaticGlobalScript(
            std::string_view module_name,
            std::string_view descriptor_key,
            std::span<const lux::meta::RefFunction* const> functions,
            std::span<const lux::script::ScriptSymbolId> symbols,
            CppStaticRecordSemanticResolver record_types
        ) noexcept
    {
        if (module_name.empty() || descriptor_key.empty() ||
            functions.size() != symbols.size())
        {
            return lux::cxx::unexpected(
                ECppStaticScriptBridgeError::INVALID_DESCRIPTOR
            );
        }
        try
        {
            auto state = std::make_unique<CppStaticScriptDescriptor::State>();
            state->description.module_name = module_name;
            state->description.body = lux::rdesc::CppStaticScript{
                std::string{descriptor_key}};
            state->descriptor_key = descriptor_key;
            state->description.exports.reserve(functions.size());
            state->callables.reserve(functions.size());
            for (std::size_t function_index{};
                 function_index < functions.size(); ++function_index)
            {
                const auto* function = functions[function_index];
                if (!function)
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::INVALID_DESCRIPTOR
                    );
                }
                auto projected = projectInvokable(
                    function->invokable,
                    symbols[function_index],
                    function->is_noexcept,
                    record_types
                );
                if (!projected)
                {
                    if (projected.error() ==
                        ECppStaticScriptBridgeError::METHOD_NOT_NOEXCEPT)
                    {
                        return lux::cxx::unexpected(
                            ECppStaticScriptBridgeError::FUNCTION_NOT_NOEXCEPT
                        );
                    }
                    return lux::cxx::unexpected(projected.error());
                }
                const auto symbol = projected->symbol_id;
                if (std::any_of(
                        state->callables.begin(),
                        state->callables.end(),
                        [symbol](const auto& value) noexcept
                        {
                            return value.symbol == symbol;
                        }))
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::DUPLICATE_SYMBOL
                    );
                }
                state->description.exports.push_back(std::move(*projected));
                state->callables.push_back(Callable{symbol, nullptr, function});
            }
            return CppStaticScriptDescriptor{std::move(state)};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                ECppStaticScriptBridgeError::ALLOCATION_FAILURE
            );
        }
    }

    struct CppStaticScriptBackend::State final
    {
        struct Instance final
        {
            const CppStaticScriptDescriptor::State* descriptor{};
            void* object{};
        };

        struct PreparedCall final
        {
            Instance* instance{};
            const Callable* callable{};
            std::vector<void*> arguments;

            static int invoke(lux_script_call_frame* frame) noexcept
            {
                if (!frame || !frame->user_context)
                    return -1;
                auto& self = *static_cast<PreparedCall*>(frame->user_context);
                const auto& invokable = self.callable->method
                    ? self.callable->method->invokable
                    : self.callable->function->invokable;
                if (frame->arg_count != invokable.parameters.size() ||
                    frame->arg_count > self.arguments.size() ||
                    (frame->arg_count != 0U && !frame->args) ||
                    (frame->return_count != 0U && !frame->returns))
                {
                    return -2;
                }
                for (std::size_t index{}; index < frame->arg_count; ++index)
                    self.arguments[index] = frame->args[index].data;
                void* result = frame->return_count == 0U
                    ? nullptr
                    : frame->returns[0].data;
                invokable.invoker(
                    self.callable->method ? self.instance->object : nullptr,
                    self.arguments.data(),
                    result
                );
                return 0;
            }
        };

        explicit State(
            std::span<const CppStaticScriptDescriptor* const> source,
            std::size_t capacity
        )
            : instance_capacity(capacity)
        {
            descriptors.assign(source.begin(), source.end());
        }

        [[nodiscard]] const CppStaticScriptDescriptor::State* find(
            const lux::asset::ScriptAssetContent& asset
        ) const noexcept
        {
            const auto* body = std::get_if<lux::rdesc::CppStaticScript>(
                std::addressof(asset.description.body)
            );
            if (!body)
                return nullptr;
            for (const auto* descriptor : descriptors)
            {
                if (!descriptor || !descriptor->state_)
                    continue;
                if (descriptor->state_->descriptor_key == body->descriptor)
                {
                    return descriptor->state_.get();
                }
            }
            const CppStaticScriptDescriptor::State* only_descriptor{};
            std::size_t valid_descriptor_count{};
            for (const auto* descriptor : descriptors)
            {
                if (descriptor && descriptor->state_)
                {
                    only_descriptor = descriptor->state_.get();
                    ++valid_descriptor_count;
                }
            }
            if (valid_descriptor_count == 1U)
                return only_descriptor;

            const CppStaticScriptDescriptor::State* candidate{};
            for (const auto* descriptor : descriptors)
            {
                if (!descriptor || !descriptor->state_ ||
                    descriptor->state_->description.exports !=
                        asset.description.exports)
                {
                    continue;
                }
                if (candidate)
                    return nullptr;
                candidate = descriptor->state_.get();
            }
            return candidate;
        }

        [[nodiscard]] static bool executableContractMatches(
            const lux::asset::ScriptAssetContent& asset,
            const CppStaticScriptDescriptor::State& descriptor
        ) noexcept
        {
            const auto* asset_body = std::get_if<lux::rdesc::CppStaticScript>(
                std::addressof(asset.description.body)
            );
            const auto* descriptor_body =
                std::get_if<lux::rdesc::CppStaticScript>(
                    std::addressof(descriptor.description.body)
                );
            return asset_body && descriptor_body &&
                asset.description.module_name ==
                    descriptor.description.module_name &&
                asset_body->descriptor == descriptor_body->descriptor &&
                asset.description.exports == descriptor.description.exports;
        }

        static EScriptBackendResult createInstance(
            void* opaque,
            const ScriptInstanceCreateContext& context,
            const lux::asset::ScriptAssetContent& asset,
            ScriptBackendInstance& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            const auto* descriptor = self.find(asset);
            if (!descriptor)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            if (!executableContractMatches(asset, *descriptor))
            {
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            }
            const bool entity_scope =
                std::holds_alternative<EntityScriptScope>(context.scope);
            if (descriptor->entity_scope != entity_scope)
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            if (self.active_instances >= self.instance_capacity)
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            auto* instance = new (std::nothrow) Instance{descriptor, nullptr};
            if (!instance)
                return EScriptBackendResult::ALLOCATION_FAILURE;
            if (descriptor->reflected_class)
            {
                const auto& type = descriptor->reflected_class->type;
                try
                {
                    instance->object = ::operator new(
                        type.size,
                        std::align_val_t{type.alignment},
                        std::nothrow
                    );
                    if (!instance->object)
                    {
                        delete instance;
                        return EScriptBackendResult::ALLOCATION_FAILURE;
                    }
                    descriptor->reflected_class->construct(instance->object);
                    if (descriptor->attach && context.behavior)
                        descriptor->attach(
                            instance->object,
                            *context.behavior
                        );
                }
                catch (...)
                {
                    if (instance->object)
                    {
                        ::operator delete(
                            instance->object,
                            std::align_val_t{type.alignment}
                        );
                    }
                    delete instance;
                    return EScriptBackendResult::CONSTRUCTION_FAILURE;
                }
            }
            ++self.active_instances;
            result.value = instance;
            return EScriptBackendResult::SUCCESS;
        }

        static EScriptBackendResult prepareMethod(
            void*,
            ScriptBackendInstance opaque_instance,
            const lux::rdesc::ScriptFunction& function,
            lux::script::BoundScriptCall& result
        ) noexcept
        {
            auto* instance = static_cast<Instance*>(opaque_instance.value);
            if (!instance)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            const auto found = std::find_if(
                instance->descriptor->callables.begin(),
                instance->descriptor->callables.end(),
                [&](const auto& callable) noexcept
                {
                    return callable.symbol == function.symbol_id;
                }
            );
            if (found == instance->descriptor->callables.end())
                return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
            try
            {
                auto call = std::make_unique<PreparedCall>();
                call->instance = instance;
                call->callable = std::addressof(*found);
                const auto& invokable = found->method
                    ? found->method->invokable
                    : found->function->invokable;
                call->arguments.resize(invokable.parameters.size());
                result = lux::script::BoundScriptCall{
                    &PreparedCall::invoke,
                    call.release()};
                return EScriptBackendResult::SUCCESS;
            }
            catch (const std::bad_alloc&)
            {
                return EScriptBackendResult::ALLOCATION_FAILURE;
            }
        }

        static void releaseMethod(
            void*,
            ScriptBackendInstance,
            lux::script::BoundScriptCall call
        ) noexcept
        {
            delete static_cast<PreparedCall*>(call.context);
        }

        static void destroyInstance(
            void* opaque,
            ScriptBackendInstance opaque_instance
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* instance = static_cast<Instance*>(opaque_instance.value);
            if (!instance)
                return;
            if (instance->object)
            {
                const auto& type = instance->descriptor->reflected_class->type;
                instance->descriptor->reflected_class->destruct(instance->object);
                ::operator delete(
                    instance->object,
                    std::align_val_t{type.alignment}
                );
            }
            delete instance;
            --self.active_instances;
        }

        std::vector<const CppStaticScriptDescriptor*> descriptors;
        std::size_t instance_capacity{};
        std::size_t active_instances{};
    };

    CppStaticScriptBackend::CppStaticScriptBackend(
        std::span<const CppStaticScriptDescriptor* const> descriptors,
        std::size_t instance_capacity
    ) noexcept
    {
        if (descriptors.empty() || instance_capacity == 0U)
            return;
        try
        {
            state_ = std::make_unique<State>(descriptors, instance_capacity);
        }
        catch (const std::bad_alloc&)
        {
        }
    }

    CppStaticScriptBackend::~CppStaticScriptBackend() = default;
    CppStaticScriptBackend::CppStaticScriptBackend(
        CppStaticScriptBackend&&
    ) noexcept = default;
    CppStaticScriptBackend&
    CppStaticScriptBackend::operator=(
        CppStaticScriptBackend&&
    ) noexcept = default;

    CppStaticScriptBackend::operator bool() const noexcept
    {
        return state_ != nullptr;
    }

    ScriptBackendDescriptor CppStaticScriptBackend::descriptor() noexcept
    {
        return state_
            ? ScriptBackendDescriptor{
                lux::rdesc::Script::Kind::CPP_STATIC,
                state_.get(),
                &State::createInstance,
                &State::prepareMethod,
                &State::releaseMethod,
                &State::destroyInstance}
            : ScriptBackendDescriptor{};
    }
}
