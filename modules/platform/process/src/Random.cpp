#include <lux/engine/platform/Random.hpp>
#include <limits>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lux::engine::platform
{
    bool fillRandomBytes(std::span<std::byte> destination) noexcept
    {
#if defined(_WIN32)
        if (destination.size() > std::numeric_limits<ULONG>::max()) return false;
        return BCryptGenRandom(
                   nullptr,
                   reinterpret_cast<PUCHAR>(destination.data()),
                   static_cast<ULONG>(destination.size()),
                   BCRYPT_USE_SYSTEM_PREFERRED_RNG
               ) == 0;
#else
        const int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) return false;
        std::size_t offset = 0;
        while (offset < destination.size())
        {
            const auto count = read(descriptor, destination.data() + offset, destination.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0)
            {
                close(descriptor);
                return false;
            }
            offset += static_cast<std::size_t>(count);
        }
        close(descriptor);
        return true;
#endif
    }
} // namespace lux::engine::platform
