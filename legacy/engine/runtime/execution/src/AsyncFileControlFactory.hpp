#pragma once

#include <lux/engine/runtime/execution/AsyncFileService.hpp>
#include <lux/engine/runtime/execution/detail/AsioConfig.hpp>

#include <asio/io_context.hpp>

#include <memory>

namespace experimental::execution
{
    struct static_thread_pool;
}

namespace lux::exec::detail
{
    [[nodiscard]] std::shared_ptr<AsyncFileControl> makeAsyncFileControl(
        asio::io_context& coordinator,
        ::experimental::execution::static_thread_pool& blocking_io);
}
