#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/error/Error.hpp>

namespace lux::editor
{
    template <class T> using FrameworkResult = cxx::expected<T, error::Error>;
}
