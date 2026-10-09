#include <lux/engine/render/graph/KernelDescriptor.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
    void require(bool value, const char* message) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::abort();
        }
    }

    void contribute(const lux::render::RGPassDescription&, lux::render::ViewArenaContribution& result)
    {
        ++result.frustum_ubo_count;
    }
} // namespace

int main()
{
    using namespace lux::render;
    auto& slots = FrameExtensionRegistry::instance();
    auto& kernels = KernelRegistry::instance();
    require(slots.idOf("mesh_instance") == kInvalidExtSlot, "loading a DLL does not register builtins");
    require(!slots.registerSlot("") && !slots.registerSlot("bad name"), "invalid names consume no slots");
    const auto first = slots.registerSlot("external.first");
    require(first && *first == 1, "explicit first extension");
    std::string name = "test.second";
    KernelDescriptor descriptor{.contribute_arena = &contribute, .ext_slot_name = name.c_str()};
    auto second = kernels.registerKernel({"TestKernel", descriptor, {}});
    require(second.has_value(), "register kernel with extension");
    const auto* stored = kernels.find(*second);
    require(stored && stored->extension_slot == 2, "kernel uses same allocator after independent registration");
    require(stored->extension_slot == slots.idOf(name), "writer and kernel agree on exact slot");
    require(kernels.registerKernel({"TestKernel", descriptor, {}}) == second, "same descriptor is idempotent");
    descriptor.emit = +[](ProgramEmitter&, std::uint32_t, const RGCompiledPass&, const RGCompiledGraph&) {};
    auto conflict = kernels.registerKernel({"TestKernel", descriptor, {}});
    require(!conflict && conflict.error() == EKernelRegistrationError::CONFLICT, "replacement rejected");
    require(kernels.find(*second) == stored && !stored->descriptor.emit, "original descriptor remains unchanged");
    name.assign(200, 'x');
    require(std::string_view(stored->descriptor.ext_slot_name) == "test.second", "registry owns extension name");
    for (unsigned i = 3; i <= kMaxFrameExtensionSlots; ++i)
    {
        auto result = slots.registerSlot("slot_" + std::to_string(i));
        require(result && *result == i, "all 32 valid nonzero slots admitted");
    }
    auto full = slots.registerSlot("overflow");
    require(!full && full.error() == EFrameExtensionRegistrationError::CAPACITY, "capacity enforced");
    require(slots.registerSlot("external.first") == first, "duplicate remains valid when full");
    auto rejected = kernels.registerKernel({"Overflow", {.ext_slot_name = "overflow"}, {}});
    require(!rejected && rejected.error() == EKernelRegistrationError::EXTENSION_CAPACITY, "kernel capacity error");
    require(kernels.idOf("Overflow") == kInvalidKernelId && !kernels.find(0), "failed kernel not published");
    require(kernels.find(*second) == stored, "published address stable after registrations");
    std::puts("PASS: sole slot authority, 32 slots, idempotence, conflict, owned name and capacity rejection");
}
