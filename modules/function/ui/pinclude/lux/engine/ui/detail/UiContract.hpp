#pragma once

#include <cstddef>
#include <cstdlib>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace lux::ui::detail
{
    [[noreturn]] inline void failUiContract() noexcept
    {
#if defined(_MSC_VER)
        __fastfail(7u);
#else
        std::abort();
#endif
    }

    [[nodiscard]] inline const void* currentUiThreadToken() noexcept
    {
        static thread_local const std::byte token{};
        return &token;
    }

    inline void requireUiOwner(std::thread::id owner, const void* owner_token) noexcept
    {
        if (currentUiThreadToken() != owner_token)
            failUiContract();
#if !defined(NDEBUG) || defined(LUX_UI_CONTRACT_CHECKS)
        if (std::this_thread::get_id() != owner)
            failUiContract();
#else
        static_cast<void>(owner);
#endif
    }

#if !defined(NDEBUG) || defined(LUX_UI_CONTRACT_CHECKS)
#define LUX_UI_CHECK_OWNER(owner_expression, owner_token_expression)                                                   \
    ::lux::ui::detail::requireUiOwner((owner_expression), (owner_token_expression))
#else
#define LUX_UI_CHECK_OWNER(owner_expression, owner_token_expression) static_cast<void>(0)
#endif
} // namespace lux::ui::detail
