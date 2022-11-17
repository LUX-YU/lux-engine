#include <clang-c/Index.h>
#include "lux-engine/core/reflection/LuxCxxParser.hpp"
#include "lux-engine/core/reflection/LuxCxxParserImpl.hpp"

namespace lux::engine::core
{
    LuxCxxParser::LuxCxxParser()
    {
        _impl = std::make_unique<LuxCxxParserImpl>();
    }

    LuxCxxParser::~LuxCxxParser() = default;


} // namespace lux::engine::core
