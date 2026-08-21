#include <lux/engine/function/script/ScriptRuntime.hpp>
#include <lux/engine/function/script/backends/LuaBackend.hpp>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace
{
    [[nodiscard]] std::span<const std::byte> bytesOf(const std::string& value)
    {
        return {
            reinterpret_cast<const std::byte*>(value.data()),
            value.size()
        };
    }
}

int main()
{
    using lux::script::EScriptError;

    lux::script::ScriptRuntime runtime;
    assert(runtime.registerBackend(lux::script::lua_backend::create()));

    const std::string source = "local value = 41 + 1";
    auto memory_module = runtime.loadModuleFromMemory(
        "lua",
        bytesOf(source),
        "memory_lua"
    );
    assert(memory_module);

    auto main_function = runtime.findFunction(memory_module.value(), "main");
    assert(main_function);

    lux_script_call_frame raw{};
    lux::script::CallFrame frame(&raw);
    assert(runtime.invoke(main_function.value(), frame));
    assert(runtime.unloadModule(memory_module.value()));

    auto stale = runtime.invoke(main_function.value(), frame);
    assert(!stale);
    assert(stale.error().code == EScriptError::STALE_HANDLE);

    const std::string invalid_source = "function (";
    auto invalid = runtime.loadModuleFromMemory(
        "lua",
        bytesOf(invalid_source),
        "invalid_lua"
    );
    assert(!invalid);
    assert(invalid.error().code == EScriptError::LOAD_FAILED);
    assert(!invalid.error().detail.empty());

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        std::filesystem::temp_directory_path()
        / ("lux_script_runtime_" + std::to_string(nonce) + ".lua");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        assert(output);
        output.write(source.data(), static_cast<std::streamsize>(source.size()));
    }

    auto file_module = runtime.loadModule(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    assert(file_module);
    assert(runtime.unloadModule(file_module.value()));

    auto missing = runtime.loadModule(path);
    assert(!missing);
    assert(missing.error().code == EScriptError::IO_ERROR);

    return 0;
}
