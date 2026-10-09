#include <lux/engine/render/graph/KernelDescriptor.hpp>

namespace
{
    unsigned* calls;

    void contribute(const lux::render::RGPassDescription&, lux::render::ViewArenaContribution& result)
    {
        ++*calls;
        result.shadow_slice_count = 27;
    }
} // namespace

#if defined(_WIN32)
#define KERNEL_PLUGIN_EXPORT __declspec(dllexport)
#else
#define KERNEL_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" KERNEL_PLUGIN_EXPORT lux::render::KernelDeclaration kernelDeclaration(unsigned* observed) noexcept
{
    calls = observed;
    return {"plugin.kernel", {.contribute_arena = &contribute, .ext_slot_name = "plugin.frame"}};
}
