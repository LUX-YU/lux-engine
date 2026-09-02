#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/editor/application/detail/PresentationThreadStart.hpp>

#include <atomic>
#include <cassert>
#include <new>
#include <system_error>
#include <thread>

int main()
{
    namespace detail = lux::editor::application::detail;

    const auto thread_failure = detail::startPresentationThread([]() -> std::jthread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    assert(!thread_failure);
    assert(thread_failure.error() == detail::EUiVulkanPresentationError::THREAD_CREATION_FAILURE);

    const auto allocation_failure = detail::startPresentationThread([]() -> std::jthread {
        throw std::bad_alloc{};
    });
    assert(!allocation_failure);
    assert(allocation_failure.error() == detail::EUiVulkanPresentationError::ALLOCATION_FAILURE);

    std::atomic_bool ran{};
    auto success = detail::startPresentationThread([&ran] {
        return std::jthread([&ran] { ran.store(true, std::memory_order_release); });
    });
    assert(success);
    success->join();
    assert(ran.load(std::memory_order_acquire));
    return 0;
}
