#include <lux/engine/function/script/ScriptRuntime.hpp>
#include <lux/engine/function/script/backends/NativeBackend.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
    [[nodiscard]] std::vector<std::byte> readFile(
        const std::filesystem::path& path
    )
    {
        std::ifstream input(path, std::ios::binary);
        assert(input);
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        assert(size >= 0);
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
        assert(input || input.eof());
        return bytes;
    }
}

int main()
{
    using lux::script::EScriptError;

    const std::filesystem::path fixture = LUX_SCRIPT_NATIVE_FIXTURE;
    const std::filesystem::path bad_abi_fixture = LUX_SCRIPT_NATIVE_BAD_ABI_FIXTURE;
    const std::filesystem::path bad_entry_fixture = LUX_SCRIPT_NATIVE_BAD_ENTRY_FIXTURE;
    const std::filesystem::path missing_entry_fixture =
        LUX_SCRIPT_NATIVE_MISSING_ENTRY_FIXTURE;
    const std::filesystem::path bind_failure_fixture =
        LUX_SCRIPT_NATIVE_BIND_FAILURE_FIXTURE;

    lux::script::ScriptRuntime runtime;
    assert(runtime.registerBackend(lux::script::native_backend::create()));

    auto loaded = runtime.loadModule(fixture);
    assert(loaded);
    auto function = runtime.findFunction(loaded.value(), "Increment");
    assert(function);

    std::int32_t counter = 0;
    lux_script_call_frame raw{};
    raw.user_context = &counter;
    lux::script::CallFrame frame(&raw);
    assert(runtime.invoke(function.value(), frame));
    assert(counter == 1);

    lux_script_call_frame failing_raw{};
    lux::script::CallFrame failing_frame(&failing_raw);
    auto invocation_failure = runtime.invoke(function.value(), failing_frame);
    assert(!invocation_failure);
    assert(invocation_failure.error().code == EScriptError::INVOKE_FAILED);

    assert(runtime.unloadModule(loaded.value()));
    auto stale = runtime.invoke(function.value(), frame);
    assert(!stale);
    assert(stale.error().code == EScriptError::STALE_HANDLE);

    const auto bytes = readFile(fixture);
    auto memory_loaded = runtime.loadModuleFromMemory(
        "native",
        bytes,
        "native_fixture_memory"
    );
    assert(memory_loaded);
    assert(runtime.unloadModule(memory_loaded.value()));

    auto bad_abi = runtime.loadModule(bad_abi_fixture);
    assert(!bad_abi);
    assert(bad_abi.error().code == EScriptError::ABI_MISMATCH);

    auto bad_entry = runtime.loadModule(bad_entry_fixture);
    assert(!bad_entry);
    assert(bad_entry.error().code == EScriptError::INVALID_MODULE);

    auto missing_entry = runtime.loadModule(missing_entry_fixture);
    assert(!missing_entry);
    assert(missing_entry.error().code == EScriptError::INVALID_ENTRY_POINT);

    auto bind_failure = runtime.loadModule(bind_failure_fixture);
    assert(!bind_failure);
    assert(bind_failure.error().code == EScriptError::HOST_BIND_FAILED);

    auto missing = runtime.loadModule(fixture.parent_path() / "missing_script.dll");
    assert(!missing);
    assert(missing.error().code == EScriptError::IO_ERROR);

    return 0;
}
