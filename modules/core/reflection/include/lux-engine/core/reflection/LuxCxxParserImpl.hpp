#pragma once
#include <lux-engine/core/reflection/libclang.hpp>
#include <lux-engine/platform/system/visibility_control.h> // TODO remove LUX_EXPORT

namespace lux::engine::core
{
    class LuxCxxParserImpl
    {
    public:
        LUX_EXPORT LuxCxxParserImpl();

        LUX_EXPORT ~LuxCxxParserImpl();

        LUX_EXPORT libclang::TranslationUnit translate(const std::string& file, const std::vector<std::string>& commands);

    private:
        // shared context for creating translation units
        CXIndex clang_index;
    };
} // namespace lux::engine::core
