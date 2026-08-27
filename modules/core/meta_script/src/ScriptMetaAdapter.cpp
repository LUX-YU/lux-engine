#include <lux/engine/meta/ScriptMetaAdapter.hpp>

#include <new>
#include <vector>

namespace lux::meta
{
    namespace
    {
        [[nodiscard]] std::string_view builtinName(EBaseType type) noexcept
        {
            switch (type)
            {
            case EBaseType::Bool: return "bool";
            case EBaseType::Int8: return "i8";
            case EBaseType::Uint8: return "u8";
            case EBaseType::Int16: return "i16";
            case EBaseType::Uint16: return "u16";
            case EBaseType::Int32: return "i32";
            case EBaseType::Uint32: return "u32";
            case EBaseType::Int64: return "i64";
            case EBaseType::Uint64: return "u64";
            case EBaseType::Float: return "f32";
            case EBaseType::Double: return "f64";
            default: return {};
            }
        }

        [[nodiscard]] lux::cxx::expected<
            lux::script::ScriptSemanticType,
            EScriptMetaAdapterError> adaptType(
                const RefType& type,
                ScriptRecordNameResolver resolver,
                bool is_return
            ) noexcept
        {
            const auto qualifier = static_cast<ETypeQual>(type.qtype.qual);
            lux::script::EScriptPassMode pass{
                lux::script::EScriptPassMode::VALUE};
            switch (qualifier)
            {
            case ETypeQual::Value:
                break;
            case ETypeQual::LRefToConst:
                if (is_return)
                {
                    return lux::cxx::unexpected(
                        EScriptMetaAdapterError::RETURN_NOT_SUPPORTED
                    );
                }
                pass = lux::script::EScriptPassMode::CONST_REF;
                break;
            case ETypeQual::LRef:
                return lux::cxx::unexpected(
                    EScriptMetaAdapterError::MUTABLE_REFERENCE_NOT_SUPPORTED
                );
            case ETypeQual::RRef:
            case ETypeQual::RRefToConst:
                return lux::cxx::unexpected(
                    EScriptMetaAdapterError::RVALUE_REFERENCE_NOT_SUPPORTED
                );
            default:
                return lux::cxx::unexpected(
                    EScriptMetaAdapterError::POINTER_NOT_SUPPORTED
                );
            }

            const auto base = static_cast<EBaseType>(type.qtype.base);
            auto canonical_name = builtinName(base);
            if (base == EBaseType::Record && resolver.canonicalName)
                canonical_name = resolver.canonicalName(resolver.context, type);
            if (canonical_name.empty())
            {
                return lux::cxx::unexpected(
                    EScriptMetaAdapterError::UNSUPPORTED_TYPE
                );
            }
            return lux::script::ScriptSemanticType{
                lux::script::scriptSemanticTypeId(canonical_name),
                canonical_name,
                pass};
        }
    }

    lux::cxx::expected<AdaptedScriptSignature, EScriptMetaAdapterError>
    adaptScriptSignature(
        const RefMethod& method,
        ScriptRecordNameResolver resolver,
        std::span<lux::script::ScriptSemanticType> parameter_storage,
        std::span<lux::script::ScriptSemanticType> return_storage
    ) noexcept
    {
        if (!method.is_noexcept)
        {
            return lux::cxx::unexpected(
                EScriptMetaAdapterError::METHOD_NOT_NOEXCEPT
            );
        }
        if (method.visibility != EVisibility::Public)
        {
            return lux::cxx::unexpected(
                EScriptMetaAdapterError::METHOD_NOT_PUBLIC
            );
        }
        if (method.invokable.is_variadic)
        {
            return lux::cxx::unexpected(
                EScriptMetaAdapterError::VARIADIC_NOT_SUPPORTED
            );
        }
        if (parameter_storage.size() < method.invokable.parameters.size())
        {
            return lux::cxx::unexpected(
                EScriptMetaAdapterError::CAPACITY_EXCEEDED
            );
        }
        for (std::size_t index{};
             index < method.invokable.parameters.size();
             ++index)
        {
            auto type = adaptType(
                method.invokable.parameters[index].type,
                resolver,
                false
            );
            if (!type)
                return lux::cxx::unexpected(type.error());
            parameter_storage[index] = *type;
        }

        std::size_t return_count{};
        const auto return_base = static_cast<EBaseType>(
            method.invokable.return_type.qtype.base
        );
        if (return_base != EBaseType::Void)
        {
            if (return_storage.empty())
            {
                return lux::cxx::unexpected(
                    EScriptMetaAdapterError::CAPACITY_EXCEEDED
                );
            }
            auto type = adaptType(
                method.invokable.return_type,
                resolver,
                true
            );
            if (!type)
                return lux::cxx::unexpected(type.error());
            return_storage[0] = *type;
            return_count = 1U;
        }
        return AdaptedScriptSignature{
            parameter_storage.first(method.invokable.parameters.size()),
            return_storage.first(return_count)};
    }

    struct ReflectedScriptCall::State final
    {
        const RefMethod* method{};
        void* object{};
        std::vector<void*> arguments;

        static int invoke(lux_script_call_frame* frame) noexcept
        {
            if (!frame || !frame->user_context)
                return -1;
            auto& self = *static_cast<State*>(frame->user_context);
            if (frame->arg_count != self.method->invokable.parameters.size() ||
                frame->arg_count > self.arguments.size() ||
                (frame->arg_count != 0U && !frame->args))
            {
                return -2;
            }
            for (std::size_t index{}; index < frame->arg_count; ++index)
                self.arguments[index] = frame->args[index].data;
            void* result = frame->return_count == 0U || !frame->returns
                ? nullptr
                : frame->returns[0].data;
            self.method->invokable.invoker(
                self.object,
                self.arguments.data(),
                result
            );
            return 0;
        }
    };

    lux::cxx::expected<ReflectedScriptCall, EScriptMetaAdapterError>
    ReflectedScriptCall::create(
        const RefMethod& method,
        void* object,
        std::size_t argument_capacity
    ) noexcept
    {
        if (!method.is_noexcept)
        {
            return lux::cxx::unexpected(
                EScriptMetaAdapterError::METHOD_NOT_NOEXCEPT
            );
        }
        if (!method.invokable.invoker)
        {
            return lux::cxx::unexpected(
                EScriptMetaAdapterError::MISSING_INVOKER
            );
        }
        if (argument_capacity < method.invokable.parameters.size())
        {
            return lux::cxx::unexpected(
                EScriptMetaAdapterError::CAPACITY_EXCEEDED
            );
        }
        try
        {
            auto state = std::make_unique<State>();
            state->method = std::addressof(method);
            state->object = object;
            state->arguments.resize(argument_capacity);
            return ReflectedScriptCall(std::move(state));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                EScriptMetaAdapterError::ALLOCATION_FAILURE
            );
        }
    }

    ReflectedScriptCall::ReflectedScriptCall(
        std::unique_ptr<State> state
    ) noexcept
        : state_(std::move(state))
    {}
    ReflectedScriptCall::ReflectedScriptCall(ReflectedScriptCall&&) noexcept =
        default;
    ReflectedScriptCall& ReflectedScriptCall::operator=(
        ReflectedScriptCall&&
    ) noexcept = default;
    ReflectedScriptCall::~ReflectedScriptCall() = default;

    lux::script::BoundScriptCall ReflectedScriptCall::boundCall() noexcept
    {
        return state_
            ? lux::script::BoundScriptCall{&State::invoke, state_.get()}
            : lux::script::BoundScriptCall{};
    }
}
