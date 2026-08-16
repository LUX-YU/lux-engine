#include <lux/engine/core/extension_abi/ModuleAbi.hpp>

#if defined(_WIN32)
#define LUX_TEST_EXTENSION_EXPORT __declspec(dllexport)
#else
#define LUX_TEST_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif

extern "C" LUX_TEST_EXTENSION_EXPORT
const lux::extensions::ExtensionModuleDescriptorV4*
luxGetExtensionModuleV4() noexcept
{
    using namespace lux::extensions;
    static constexpr ExtensionModuleDescriptorV4 descriptor{
        sizeof(ExtensionModuleDescriptorV4),
        kExtensionAbiV4,
        kEngineExtensionAbiFingerprint,
        lux::cxx::AbiStringView{"org.lux.test.minimal"},
        ExtensionVersion{1u, 2u, 3u},
        EExtensionModuleTarget::RUNTIME,
        nullptr,
        0u};
    return &descriptor;
}
