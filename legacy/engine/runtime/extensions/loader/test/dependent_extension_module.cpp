#include <lux/engine/extensions/ExtensionAbi.hpp>

#include <array>

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
    static constexpr std::array dependencies{
        ExtensionDependencyView{
            lux::cxx::AbiStringView{"org.lux.test.minimal"},
            1u,
            1u}};
    static constexpr ExtensionModuleDescriptorV5 descriptor{
        sizeof(ExtensionModuleDescriptorV5),
        kExtensionAbiV5,
        kEngineExtensionAbiFingerprint,
        lux::cxx::AbiStringView{"org.lux.test.dependent"},
        ExtensionVersion{1u, 0u, 0u},
        EExtensionModuleTarget::RUNTIME,
        dependencies.data(),
        dependencies.size()};
    return &descriptor;
}
