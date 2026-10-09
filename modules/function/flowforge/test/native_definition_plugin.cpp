#include <lux/engine/flowforge/NativeCallDefinition.hpp>

#include <cstdlib>
#include <string>

namespace
{
    void invoke(void*, void** arguments, void* result)
    {
        *static_cast<int*>(result) = *static_cast<int*>(arguments[0]) + 17;
    }
} // namespace

#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" TEST_EXPORT void createNativeDefinition(
    std::shared_ptr<const lux::flowforge::NativeCallDefinition>& output,
    lux::object::CodeLease code
) noexcept
{
    using namespace lux;
    std::string name{"plugin_compute"};
    std::string parameter{"argument"};
    std::string signature{"int(int)"};
    std::string type_name{"int"};
    auto type = meta::ref_type_of_v<int>;
    type.name = type_name;
    meta::RefInvokable info;
    info.name = name;
    info.full_name = name;
    info.type_signature = signature;
    info.return_type = type;
    info.parameters.push_back({parameter, type, type_name, type.hash, false});
    info.invoker = &invoke;
    auto definition = flowforge::NativeCallDefinition::create(info, std::move(code));
    if (!definition)
    {
        std::abort();
    }
    output = std::move(*definition);
}
