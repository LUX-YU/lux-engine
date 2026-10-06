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

extern "C" TEST_EXPORT lux::error::Error plugin_failure() noexcept
{
    using namespace lux::error;
    return makeError({"fixture.plugin.failure", "Plugin result {0}", ERecovery::NEEDS_INPUT, {EArgument::SIGNED}},
        {static_cast<std::uint64_t>(-73)});
}
