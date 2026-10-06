#pragma once
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/cxx/compile_time/expected.hpp>

namespace lux::editor
{
    template <class T> using FrameworkResult = cxx::expected<T, error::Error>;
}
