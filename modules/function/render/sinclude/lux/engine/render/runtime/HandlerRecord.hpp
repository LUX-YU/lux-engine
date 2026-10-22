#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <memory>
#include <string>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::runtime_detail
{
    // The sole Handler record, not an intermediate definition. Registration retains the factory;
    // activation creates one stable object. It survives binding changes and all accepted senders.
    struct HandlerRecord final
    {
        using Object = std::unique_ptr<void, void (*)(void*) noexcept>;

        cxx::TypeToken type;
        std::string name;
        std::string instance_name;
        void* (*create)(ResourceRegistry&) noexcept;
        Object object;
    };
} // namespace lux::render::runtime_detail
