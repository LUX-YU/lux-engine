#include <clang-c/Index.h>
#include "lux/reflection/LuxCxxParser.hpp"
#include "lux/reflection/LuxCxxParserImpl.hpp"

namespace lux::reflection
{
    LuxCxxParser::LuxCxxParser()
    {
        _impl = std::make_unique<LuxCxxParserImpl>();
    }

    LuxCxxParser::~LuxCxxParser() = default;
} // namespace lux::engine::core
