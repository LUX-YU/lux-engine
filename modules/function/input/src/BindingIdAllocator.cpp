#include <lux/engine/input/BindingIdAllocator.hpp>
#include <atomic>

namespace lux::input
{
    BindingId BindingIdAllocator::next() noexcept
    {
        static std::atomic<BindingId> counter{0};
        return ++counter;
    }

} // namespace lux::input
