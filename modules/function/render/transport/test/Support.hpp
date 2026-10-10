#pragma once

#include "TestOps.ops.hpp"
#include <lux/engine/render/transport/Transport.hpp>
#include <cstdio>
#include <cstdlib>

namespace transport_test
{
    using namespace lux::render;

    inline void check(bool passed, const char* name) noexcept
    {
        if (!passed)
        {
            std::fprintf(stderr, "FAIL: %s\n", name);
            std::abort();
        }
    }

    template <typename T>
    T must(RenderResult<T> result) noexcept
    {
        check(bool(result), "expected success");
        if constexpr (!std::is_void_v<T>) return std::move(*result);
    }

    template <typename T>
    bool fails(const RenderResult<T>& result, lux::error::ErrorId error) noexcept
    {
        return !result && result.error().type == error;
    }

    struct Fixture final
    {
        RenderRouteTable table{16};
        BoundRenderRoute<Value> value{must(table.registerOperation<Value>())};
        BoundRenderRoute<Bulk> bulk{must(table.registerOperation<Bulk>())};
        BoundRenderRoute<Blob> blob{must(table.registerOperation<Blob>())};
        BoundRenderRoute<Ping> ping{must(table.registerOperation<Ping>())};
        BoundRenderRoute<Upload> upload{must(table.registerOperation<Upload>())};
        BoundRenderRoute<Load> load{must(table.registerOperation<Load>())};
        std::unique_ptr<RenderTransport> transport;

        explicit Fixture(RenderTransportCapacity capacity = {}) noexcept
            : transport(must(RenderTransport::create(std::move(table), capacity))) {}
    };
}
