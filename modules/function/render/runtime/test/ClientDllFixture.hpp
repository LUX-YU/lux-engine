#pragma once

#include <lux/engine/render/runtime/CommandValue.hpp>
#include <cstdlib>
#include <vector>

namespace client_dll_test
{
    struct Batch final
    {
        std::uint64_t identity = 0;
        std::vector<std::uint32_t> values;
        int* destroyed = nullptr;
        const bool* pin_alive = nullptr;

        Batch() noexcept = default;
        Batch(Batch&& other) noexcept
            : identity(other.identity), values(std::move(other.values)),
              destroyed(std::exchange(other.destroyed, nullptr)), pin_alive(other.pin_alive) {}
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        Batch& operator=(Batch&&) = delete;

        ~Batch() noexcept
        {
            if (destroyed)
            {
                if (!*pin_alive) std::_Exit(EXIT_FAILURE);
                ++*destroyed;
            }
        }
    };

    struct Command final
    {
        static constexpr std::string_view name{"test.client.dll.batch"};
        using Request = Batch;
        using Response = Batch;
    };

    struct WrongRequest final
    {
        std::uint64_t identity = 0;
        std::vector<std::uint32_t> values;
        int* destroyed = nullptr;
        const bool* pin_alive = nullptr;
    };
    static_assert(sizeof(WrongRequest) == sizeof(Batch) && alignof(WrongRequest) == alignof(Batch));
    struct WrongCommand final
    {
        static constexpr std::string_view name = Command::name;
        using Request = WrongRequest;
        using Response = Batch;
    };
}

namespace lux::render
{
    template <> struct CommandValueTraits<client_dll_test::Batch>
    {
        static RenderResult<std::size_t> retainedBytes(const client_dll_test::Batch& value) noexcept
        {
            if (value.values.capacity() > (SIZE_MAX - sizeof(value)) / sizeof(std::uint32_t))
                return cxx::unexpected(RenderError{kProtocolContract});
            return sizeof(value) + value.values.capacity() * sizeof(std::uint32_t);
        }
    };
}
