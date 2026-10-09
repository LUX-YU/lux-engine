#include "registered_value_definition.hpp"
#include <lux/engine/flowforge/ScalarNodes.hpp>

#if defined(_WIN32)
#define FLOW_TEST_EXPORT __declspec(dllexport)
#else
#define FLOW_TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" FLOW_TEST_EXPORT void registerPolynomial(
    lux::flowforge::FlowNodeRegistration& output,
    const lux::object::CodeLease& code
) noexcept
{
    output = flow_test::registration(code);
}

extern "C" FLOW_TEST_EXPORT void makeDefinition(
    std::shared_ptr<const lux::flowforge::FlowNodeType>& output,
    const lux::object::CodeLease& code
) noexcept
{
    lux::flowforge::FlowNodeCatalog catalog;
    const auto definition = flow_test::registration(code);
    if (!catalog.add(std::span{&definition, 1}))
    {
        std::abort();
    }
    output = catalog.find(definition.identity.id);
}

extern "C" FLOW_TEST_EXPORT void makeScalarDefinition(
    std::shared_ptr<const lux::flowforge::FlowNodeType>& output,
    const lux::object::CodeLease& code
) noexcept
{
    lux::flowforge::FlowNodeCatalog catalog;
    if (!catalog.add(lux::flowforge::scalarNodeRegistrations(code)))
    {
        std::abort();
    }
    output = catalog.find(lux::graph::nodeTypeId("lux.flow.add"));
}
