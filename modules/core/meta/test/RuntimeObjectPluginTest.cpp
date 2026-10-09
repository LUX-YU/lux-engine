#include "RuntimeObjectPlugin.hpp"

#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/meta/RuntimeObject.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

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
    require(argc == 2, "plugin path");
    using lux::engine::platform::DynamicLibrary;
    using lux::meta::ReflectionRegistry;
    RuntimeObjectProbe probe;
    auto library = std::make_shared<DynamicLibrary>(std::filesystem::path{argv[1]});
    require(library->is_loaded(), "load real DLL");
    std::weak_ptr<DynamicLibrary> observed = library;
    auto* entry = library->get_symbol<RuntimeObjectPluginEntry>("runtimeObjectRegistration");
    require(entry != nullptr, "DLL entry");
    lux::meta::meta_module_init();
    {
        auto draft = ReflectionRegistry::beginDraft();
        require(draft.append(entry(&probe), library).has_value(), "append code owner");
        require(draft.prepareCommit().has_value(), "prepare registry");
        require(draft.commit().has_value(), "commit registry");
    }
    const auto* cls = ReflectionRegistry::instance().findClass("test.runtime.PluginValue");
    require(cls != nullptr, "published metadata");
    {
        auto original = lux::meta::RuntimeObject::create(cls);
        require(original.has_value(), "DLL construct");
        library.reset();
        require(!observed.expired(), "registry retains exact code owner");
        auto copied = original->clone();
        require(copied.has_value(), "DLL copy after external owner released");
        require(copied->data() != original->data(), "two real owners");
        original->reset();
        require(probe.destroyed == 1 && probe.destruct_tail == 1, "first DLL destructor tail");
        require(!observed.expired(), "code retained while clone lives");
    }
    require(probe.constructed == 1 && probe.copied == 1, "one construct and one copy");
    require(probe.destroyed == 2 && probe.destruct_tail == 2, "both complete DLL destructors");
    require(!observed.expired(), "metadata still owns code");
    lux::meta::meta_module_deinit();
    require(observed.expired(), "registry releases final code owner after values");
    std::puts("PASS: actual DLL construct/copy/destruct tails and registry code-owner release");
}
