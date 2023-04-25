#pragma once
#include <lux/reflection/libclang.hpp>
#include <lux/system/visibility_control.h> // TODO remove LUX_EXPORT

namespace lux::reflection
{
    class LuxCxxParserImpl
    {
    public:
        LUX_EXPORT LuxCxxParserImpl();

        LUX_EXPORT ~LuxCxxParserImpl();

        LUX_EXPORT TranslationUnit translate(const std::string& file, const std::vector<std::string>& commands);

    private:
        // shared context for creating translation units
        CXIndex clang_index;
    };
} // namespace lux::reflection
