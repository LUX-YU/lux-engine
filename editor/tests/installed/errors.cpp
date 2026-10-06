#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

#include <cassert>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    using namespace lux::error;
    assert(argc == 2);
    Error error;
    const ErrorDefinition* description{};
    {
        lux::engine::platform::DynamicLibrary library(argv[1]);
        assert(library.is_loaded());
        const auto registry = library.get_symbol<ErrorRegistry*() noexcept>("error_registry");
        const auto failure = library.get_symbol<Error() noexcept>("plugin_failure");
        assert(registry && failure);
        assert(registry() == &ErrorRegistry::instance());
        error = failure();
        description = ErrorRegistry::instance().find(error.type);
        assert(description && description->name == "fixture.plugin.failure");
    }
    assert(format(error).find("-73") != std::string::npos);
    std::vector<std::thread> workers;
    for (int i{}; i < 8; ++i)
    {
        workers.emplace_back(
            [i]
            {
                for (int n{}; n < 128; ++n)
                {
                    const auto name = "sdk.thread." + std::to_string(i) + "." + std::to_string(n);
                    assert(ErrorRegistry::instance().registerType({name, "Stable"}));
                }
            }
        );
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    assert(ErrorRegistry::instance().find(error.type) == description);
    assert(format(error).find("-73") != std::string::npos);
}
