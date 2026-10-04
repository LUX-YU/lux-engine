#pragma once
#include <lux/engine/services/ServiceRegistry.hpp>
#include <thread>

namespace fixture
{
    struct Trace final
    {
        std::thread::id owner{std::this_thread::get_id()};
        int created{}, destroyed{}, returned{}, unloaded{};
    };
    struct Value
    {
        virtual int read() const noexcept = 0;

    protected:
        ~Value() = default;
    };
} // namespace fixture
