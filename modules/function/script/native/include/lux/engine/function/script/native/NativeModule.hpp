#pragma once

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/function/script/ScriptResult.hpp>
#include <lux/engine/function/script/ScriptSemantic.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace lux::script
{
    using HostSymbolResolver =
        lux::cxx::move_only_function<void*(std::string_view symbol)>;

    class LUX_FUNCTION_PUBLIC NativeModule final
    {
    public:
        struct State;

        explicit NativeModule(std::unique_ptr<State> state) noexcept;
        NativeModule(NativeModule&&) noexcept;
        NativeModule& operator=(NativeModule&&) noexcept;
        ~NativeModule();

        NativeModule(const NativeModule&) = delete;
        NativeModule& operator=(const NativeModule&) = delete;

        [[nodiscard]] std::string_view name() const noexcept;
        [[nodiscard]] std::span<const lux_script_function_desc>
            functions() const noexcept;
        [[nodiscard]] const lux_script_function_desc*
            findFunction(std::string_view name) const noexcept;
        [[nodiscard]] const lux_script_function_desc*
            findFunction(ScriptSymbolId symbol) const noexcept;
        [[nodiscard]] std::uint32_t abiVersion() const noexcept;

    private:
        std::unique_ptr<State> state_;
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC ScriptResult<NativeModule>
        loadNativeModule(
            const std::filesystem::path& path,
            HostSymbolResolver resolver = {}
        );

    [[nodiscard]] LUX_FUNCTION_PUBLIC ScriptResult<NativeModule>
        loadNativeModule(
            std::span<const std::byte> image,
            std::string_view module_name,
            HostSymbolResolver resolver = {}
        );
}
