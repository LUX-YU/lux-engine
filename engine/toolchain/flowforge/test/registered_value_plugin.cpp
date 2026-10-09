#include "registered_value_definition.hpp"

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
