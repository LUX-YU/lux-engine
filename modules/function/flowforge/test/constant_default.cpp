#include <lux/engine/flowforge/graph/NodeBase.hpp>

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
        lux::flowforge::Node node{lux::flowforge::ENodeOperation::GET_OBJECT};
        lux::flowforge::DataInPin number{&node, {"Number", &lux::meta::ref_type_of_v<int>}, true};
        require(number.validConstant());
        require(number.constantData().get<int>() == 0);
        require(number.setConstantData(RuntimeObject{42}));
        require(number.resetConstantData().has_value());
        require(number.constantData().get<int>() == 0);

        const auto* cls = lux::meta::ReflectionRegistry::instance().findClass("std::string");
        require(cls != nullptr);
        lux::flowforge::DataInPin text{&node, {"Text", &cls->type}, true};
        require(!text.validConstant());
        auto initial = RuntimeObject::create(std::string{"keep previous value"});
        require(initial.has_value());
        require(text.setConstantData(std::move(*initial)));
        auto reset = text.resetConstantData();
        require(!reset && reset.error() == ERuntimeObjectError::DEFAULT_UNAVAILABLE);
        require(*static_cast<const std::string*>(text.constantData().data()) == "keep previous value");

        lux::flowforge::DataInPin untyped{&node, {"Untyped", nullptr}};
        require(!untyped.validConstant());
        auto invalid = untyped.resetConstantData();
        require(!invalid && invalid.error() == ERuntimeObjectError::INVALID_TYPE);
    }
    lux::meta::meta_module_deinit();
    std::puts("PASS: Flow optional defaults and explicit failure preserve existing literal");
}
