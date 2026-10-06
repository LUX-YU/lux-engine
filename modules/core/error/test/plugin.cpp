#include <lux/engine/error/ErrorRegistry.hpp>

#if defined(_WIN32)
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" TEST_EXPORT lux::error::ErrorRegistry* error_registry() noexcept
{
    return &lux::error::ErrorRegistry::instance();
}

extern "C" TEST_EXPORT lux::error::Error register_errors() noexcept
{
    using namespace lux::error;
    const ErrorDescriptor
        definition{"fixture.plugin.failure", "Plugin result {0}", ERecovery::NEEDS_INPUT, {EArgument::SIGNED}};
    const auto result = ErrorRegistry::instance().registerTypes(std::span{&definition, 1});
    return result ? Error{} : result.error();
}
extern "C" TEST_EXPORT lux::error::Error plugin_failure() noexcept
{
    return {lux::error::errorId("fixture.plugin.failure"), {static_cast<std::uint64_t>(-73)}};
}
