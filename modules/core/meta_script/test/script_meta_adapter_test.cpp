#include <lux/engine/meta/ScriptMetaAdapter.hpp>

#include <array>
#include <cassert>
#include <cstdint>

namespace
{
    struct Target final
    {
        std::int32_t base{};
    };

    void invokeAdd(void* object, void** arguments, void* result)
    {
        const auto value = *static_cast<const std::int32_t*>(arguments[0]);
        *static_cast<std::int32_t*>(result) =
            static_cast<Target*>(object)->base + value;
    }

    lux::meta::RefMethod makeMethod()
    {
        lux::meta::RefMethod method;
        method.visibility = lux::meta::EVisibility::Public;
        method.is_noexcept = true;
        method.invokable.return_type =
            lux::meta::ref_type_of_v<std::int32_t>;
        method.invokable.parameters.push_back(lux::meta::RefParam{
            "value",
            lux::meta::ref_type_of_v<const std::int32_t&>,
            "std::int32_t",
            0U,
            false});
        method.invokable.invoker = &invokeAdd;
        return method;
    }
}

int main()
{
    using namespace lux::meta;

    auto method = makeMethod();
    std::array<lux::script::ScriptSemanticType, 1U> parameters;
    std::array<lux::script::ScriptSemanticType, 1U> returns;
    auto signature = adaptScriptSignature(
        method,
        {},
        parameters,
        returns
    );
    assert(signature);
    assert(signature->parameters.size() == 1U);
    assert(signature->parameters[0].canonical_name == "i32");
    assert(
        signature->parameters[0].pass ==
        lux::script::EScriptPassMode::CONST_REF
    );
    assert(signature->returns.size() == 1U);
    assert(signature->returns[0].canonical_name == "i32");

    auto throwing = method;
    throwing.is_noexcept = false;
    auto rejected = adaptScriptSignature(throwing, {}, parameters, returns);
    assert(!rejected);
    assert(
        rejected.error() == EScriptMetaAdapterError::METHOD_NOT_NOEXCEPT
    );

    auto mutable_reference = method;
    mutable_reference.invokable.parameters[0].type =
        ref_type_of_v<std::int32_t&>;
    rejected = adaptScriptSignature(
        mutable_reference,
        {},
        parameters,
        returns
    );
    assert(!rejected);
    assert(
        rejected.error() ==
        EScriptMetaAdapterError::MUTABLE_REFERENCE_NOT_SUPPORTED
    );

    auto pointer = method;
    pointer.invokable.parameters[0].type =
        ref_type_of_v<const std::int32_t*>;
    rejected = adaptScriptSignature(pointer, {}, parameters, returns);
    assert(!rejected);
    assert(rejected.error() == EScriptMetaAdapterError::POINTER_NOT_SUPPORTED);

    Target target{5};
    auto prepared = ReflectedScriptCall::create(method, &target, 1U);
    assert(prepared);
    auto call = prepared->boundCall();
    assert(call);
    std::int32_t argument_value{7};
    std::int32_t return_value{};
    lux_script_value_slot argument{
        LUX_SCRIPT_VK_INT32,
        {},
        sizeof(argument_value),
        lux::script::scriptSemanticTypeId("i32"),
        &argument_value};
    lux_script_value_slot result{
        LUX_SCRIPT_VK_INT32,
        {},
        sizeof(return_value),
        lux::script::scriptSemanticTypeId("i32"),
        &return_value};
    lux_script_call_frame frame{
        &argument, 1U, 0U, &result, 1U, 0U, nullptr, call.context};
    assert(call.invoke(&frame) == 0);
    assert(return_value == 12);
    return 0;
}
