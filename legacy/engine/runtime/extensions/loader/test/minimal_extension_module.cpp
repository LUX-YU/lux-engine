#include <lux/engine/extensions/ExtensionAbi.hpp>

#if defined(_WIN32)
#define LUX_TEST_EXTENSION_EXPORT __declspec(dllexport)
#else
#define LUX_TEST_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif

extern "C" LUX_TEST_EXTENSION_EXPORT
const lux::extensions::ExtensionModuleDescriptorV5*
luxGetExtensionModuleV5() noexcept
{
    using namespace lux::extensions;
    static constexpr ExtensionModuleDescriptorV5 descriptor{
        sizeof(ExtensionModuleDescriptorV5),
        kExtensionAbiV5,
        kEngineExtensionAbiFingerprint,
        lux::cxx::AbiStringView{"org.lux.test.minimal"},
        ExtensionVersion{1u, 2u, 3u},
        EExtensionModuleTarget::RUNTIME,
        nullptr,
        0u};
    return &descriptor;
}
