#pragma once

// This header must precede every standalone-Asio include in lux-engine.
#include <asio/detail/throw_exception.hpp>

#include <exception>

namespace asio::detail
{
    /// ASIO_NO_EXCEPTIONS asks the application to supply this boundary. All
    /// engine calls use error_code overloads, so reaching it is a programming
    /// or allocation failure rather than a recoverable IO result.
    template <typename Exception>
    inline void throw_exception(
        const Exception& ASIO_SOURCE_LOCATION_PARAM)
    {
        std::terminate();
    }
}
