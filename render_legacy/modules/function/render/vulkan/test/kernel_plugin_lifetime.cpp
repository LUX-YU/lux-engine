#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

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
} // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "DLL argument");
    using namespace lux::render;
    using lux::engine::platform::DynamicLibrary;
    unsigned calls{};
    auto library = std::make_shared<DynamicLibrary>(std::filesystem::path{argv[1]});
    require(library->is_loaded(), "real DLL loaded");
    std::weak_ptr<DynamicLibrary> observed = library;
    using Entry = KernelDeclaration(unsigned*) noexcept;
    auto* entry = library->get_symbol<Entry>("kernelDeclaration");
    require(entry != nullptr, "real DLL declaration export");
    const auto declaration = entry(&calls);
    auto& registry = KernelRegistry::instance();
    const auto id = registry.registerKernel({declaration.canonical_name, declaration.descriptor, library});
    require(id.has_value(), "accept actual DLL kernel");
    library.reset();
    require(!observed.expired(), "registry pins code after caller drops its owner");
    const auto* kernel = registry.find(*id);
    RGPassDescription pass;
    ViewArenaContribution contribution;
    kernel->descriptor.contribute_arena(pass, contribution);
    require(calls == 1 && contribution.shadow_slice_count == 27, "real DLL function still executes");
    require(kernel->extension_slot == FrameExtensionRegistry::instance().idOf("plugin.frame"), "same extension");
    require(registry.registerKernels(std::span{&declaration, 1}).has_value(), "duplicate cannot discard original pin");
    require(!observed.expired(), "accepted pin remains until registry teardown; no unload API");
    std::puts("PASS: real DLL kernel invocation after external owner release; registry retains exact code allocation");
}
