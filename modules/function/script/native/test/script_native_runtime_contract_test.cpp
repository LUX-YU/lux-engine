#include <lux/engine/function/script/native/NativeModule.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
    [[nodiscard]] std::vector<std::byte> readFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        assert(input);
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        assert(size >= 0);
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        assert(input || input.eof());
        return bytes;
    }
}

int
main()
{
    using lux::script::EScriptError;

    const std::filesystem::path fixture = LUX_SCRIPT_NATIVE_FIXTURE;
    const std::filesystem::path bad_abi_fixture = LUX_SCRIPT_NATIVE_BAD_ABI_FIXTURE;
    const std::filesystem::path bad_entry_fixture = LUX_SCRIPT_NATIVE_BAD_ENTRY_FIXTURE;
    const std::filesystem::path missing_entry_fixture = LUX_SCRIPT_NATIVE_MISSING_ENTRY_FIXTURE;
    const std::filesystem::path bind_failure_fixture = LUX_SCRIPT_NATIVE_BIND_FAILURE_FIXTURE;

    auto loaded = lux::script::loadNativeModule(fixture);
    assert(loaded);
    const auto* function = loaded.value().findFunction("Increment");
    assert(function && function->invoke);
    assert(loaded.value().findFunction(1U) == function);
    assert(loaded.value().findFunction("OnUpdate") == nullptr);
    assert(loaded.value().findFunction(2U) != nullptr);
    assert(loaded.value().findFunction(3U) != nullptr);
    assert(loaded.value().findFunction(0U) == nullptr);
    assert(loaded.value().abiVersion() == LUX_SCRIPT_ABI_VERSION);

    std::int32_t counter = 0;
    lux_script_call_frame raw{};
    raw.user_context = &counter;
    assert(function->invoke(&raw) == 0);
    assert(counter == 1);

    lux_script_call_frame failing_raw{};
    assert(function->invoke(&failing_raw) != 0);

    const auto bytes = readFile(fixture);
    auto memory_loaded = lux::script::loadNativeModule(bytes, "native_fixture_memory");
    assert(memory_loaded);
    assert(memory_loaded.value().findFunction("Increment") != nullptr);

    auto bad_abi = lux::script::loadNativeModule(bad_abi_fixture);
    assert(!bad_abi);
    assert(bad_abi.error().code == EScriptError::ABI_MISMATCH);

    auto bad_entry = lux::script::loadNativeModule(bad_entry_fixture);
    assert(!bad_entry);
    assert(bad_entry.error().code == EScriptError::INVALID_MODULE);

    auto missing_entry = lux::script::loadNativeModule(missing_entry_fixture);
    assert(!missing_entry);
    assert(missing_entry.error().code == EScriptError::INVALID_ENTRY_POINT);

    auto bind_failure = lux::script::loadNativeModule(bind_failure_fixture);
    assert(!bind_failure);
    assert(bind_failure.error().code == EScriptError::HOST_BIND_FAILED);

    auto missing = lux::script::loadNativeModule(fixture.parent_path() / "missing_script.dll");
    assert(!missing);
    assert(missing.error().code == EScriptError::IO_ERROR);

    return 0;
}
