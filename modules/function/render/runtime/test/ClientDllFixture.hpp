#pragma once

#include <lux/engine/render/protocol/Protocol.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <cstdlib>
#include <vector>

namespace client_dll_test
{
    struct Batch final
    {
        std::uint64_t identity = 0;
        std::vector<std::uint32_t> values;
        int* destroyed = nullptr;
        const bool* host_loaded = nullptr;

        Batch() noexcept = default;
        Batch(Batch&& other) noexcept
            : identity(other.identity), values(std::move(other.values)),
              destroyed(std::exchange(other.destroyed, nullptr)), host_loaded(other.host_loaded) {}
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        Batch& operator=(Batch&&) = delete;

        ~Batch() noexcept
        {
            if (destroyed)
            {
                if (!*host_loaded) std::_Exit(EXIT_FAILURE);
                ++*destroyed;
            }
        }
    };

    struct Command final
    {
        static constexpr std::string_view name{"test.client.dll.batch"};
        using Request = Batch;
        using Response = lux::render::RenderResult<Batch>;
    };

    struct WrongRequest final
    {
        std::uint64_t identity = 0;
        std::vector<std::uint32_t> values;
        int* destroyed = nullptr;
        const bool* host_loaded = nullptr;
    };
    static_assert(sizeof(WrongRequest) == sizeof(Batch) && alignof(WrongRequest) == alignof(Batch));
    struct WrongCommand final
    {
        static constexpr std::string_view name = Command::name;
        using Request = WrongRequest;
        using Response = lux::render::RenderResult<Batch>;
    };
}
