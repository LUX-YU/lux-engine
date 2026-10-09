#include <lux/engine/render/graph/KernelDescriptor.hpp>

#include <array>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

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
    std::barrier start{5};
    std::atomic<bool> finished{};
    std::array<std::thread, 4> writers;
    for (unsigned owner{}; owner < writers.size(); ++owner)
    {
        writers[owner] = std::thread(
            [&, owner]
            {
                start.arrive_and_wait();
                for (unsigned index{}; index < 16; ++index)
                {
                    const auto name = "owner_" + std::to_string(owner) + "_" + std::to_string(index);
                    const auto result = kernels.registerKernel({name, {.ext_slot_name = "test.second"}, {}});
                    require(result.has_value(), "concurrent render owners admit independent kernels");
                    require(kernels.find(*result)->extension_slot == 2, "concurrent owner uses the original slot");
                    require(
                        slots.registerSlot("test.second").value() == 2,
                        "concurrent duplicate at full slot capacity"
                    );
                    require(kernels.registerKernel({"shared", {}, {}}).has_value(), "concurrent duplicate kernel");
                }
            }
        );
    }
    std::thread reader(
        [&]
        {
            start.arrive_and_wait();
            do
            {
                require(kernels.find(*second) == stored, "reader retains original published object");
                kernels.forEach(
                    [&](KernelTypeId id, const RegisteredKernel& entry)
                    { require(kernels.find(id) == &entry, "published prefix never contains a partial entry"); }
                );
            } while (!finished.load(std::memory_order_acquire));
        }
    );
    for (auto& writer : writers)
    {
        writer.join();
    }
    finished.store(true, std::memory_order_release);
    reader.join();
    std::size_t count{};
    kernels.forEach([&](KernelTypeId, const RegisteredKernel&) { ++count; });
    require(count == 66, "four render owners plus duplicate admission retain exactly one shared kernel");
    for (unsigned index = static_cast<unsigned>(count); index < 255; ++index)
    {
        require(kernels.registerKernel({"fill_kernel_" + std::to_string(index), {}, {}}).has_value(), "kernel range");
    }
    const auto exhausted = kernels.registerKernel({"full_kernel", {}, {}});
    require(!exhausted && exhausted.error() == EKernelRegistrationError::CAPACITY, "KernelTypeId cannot wrap");
    require(kernels.registerKernel({"shared", {}, {}}).has_value(), "kernel duplicate at full capacity");
    std::puts("PASS: sole slots, capacity, conflict, owned names, four concurrent owners and stable reader prefix");
}
