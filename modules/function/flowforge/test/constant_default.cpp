#include <lux/engine/flowforge/graph/FlowNode.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
    void require(bool value) noexcept
    {
        if (!value)
        {
            std::abort();
        }
    }
} // namespace

int main()
{
    using lux::meta::ERuntimeObjectError;
    using lux::meta::RuntimeObject;
    lux::meta::meta_module_init();
    {
        lux::flowforge::FlowPinPayload
            number{"Number", lux::flowforge::EFlowPinRole::DATA, &lux::meta::ref_type_of_v<int>, true};
        require(number.resetDefault().has_value());
        require(static_cast<bool>(number.default_value));
        require(number.default_value.get<int>() == 0);
        require(number.setDefault(RuntimeObject{42}));
        require(number.resetDefault().has_value());
        require(number.default_value.get<int>() == 0);

        const auto* cls = lux::meta::ReflectionRegistry::instance().findClass("std::string");
        require(cls != nullptr);
        lux::flowforge::FlowPinPayload text{"Text", lux::flowforge::EFlowPinRole::DATA, &cls->type, true};
        require(!static_cast<bool>(text.default_value));
        auto initial = RuntimeObject::create(std::string{"keep previous value"});
        require(initial.has_value());
        require(text.setDefault(std::move(*initial)));
        auto reset = text.resetDefault();
        require(!reset && reset.error() == ERuntimeObjectError::DEFAULT_UNAVAILABLE);
        require(*static_cast<const std::string*>(text.default_value.data()) == "keep previous value");

        lux::flowforge::FlowPinPayload untyped{"Untyped"};
        require(!static_cast<bool>(untyped.default_value));
        auto invalid = untyped.resetDefault();
        require(!invalid && invalid.error() == ERuntimeObjectError::INVALID_TYPE);
    }
    lux::meta::meta_module_deinit();
    std::puts("PASS: Flow optional defaults and explicit failure preserve existing literal");
}
