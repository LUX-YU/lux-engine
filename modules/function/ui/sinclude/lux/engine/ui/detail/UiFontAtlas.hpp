#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/UiFontSource.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace lux::ui::detail
{
struct UiFontAtlasSnapshot final
{
    std::vector<std::uint8_t> pixels;
    int width{};
    int height{};
};

using UiFontAtlasResult = lux::cxx::expected<UiFontAtlasSnapshot, EUiInitError>;
} // namespace lux::ui::detail
